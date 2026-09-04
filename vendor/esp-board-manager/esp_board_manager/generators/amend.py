# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

"""
Amend support for ESP Board Manager.

This module implements the ``-a/--amend`` mechanism: a single entry directory
that contains a ``board_amend.yaml`` manifest describing an ordered list of
fragments to apply on top of a selected base board.

Public API:
    AmendError               - exception raised for any amend-time misuse
    AmendFragment            - one resolved manifest item
    AmendPlan                - parsed manifest + classified fragments
    build_amend_plan()       - parse + validate the manifest, return an AmendPlan
    apply_amend_plan_to_yaml() - merge plan's YAML fragments into base YAML data

Design rule: ``apply:`` in ``board_amend.yaml`` is the *single* source of
truth. No file is auto-discovered, directory items are rejected, and every
file (including ``sdkconfig.defaults.board`` / ``Kconfig.projbuild``) must
be listed explicitly. See ``docs/zh_CN/create-board/amend.rst`` for the full spec.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

from .utils.logger import LoggerMixin, get_logger
from .utils.yaml_utils import (
    BoardConfigYamlError,
    load_board_yaml_file,
    load_yaml_mapping_from_text,
)


MANIFEST_FILENAME = 'board_amend.yaml'
SDKCONFIG_DEFAULTS_BOARD_FILE = 'sdkconfig.defaults.board'
KCONFIG_PROJBUILD_FILE = 'Kconfig.projbuild'

YAML_SUFFIXES = frozenset({'.yaml', '.yml'})
SOURCE_SUFFIXES = frozenset({'.c', '.cpp', '.cc', '.cxx', '.s'})
HEADER_SUFFIXES = frozenset({'.h', '.hpp'})
SUPPORTED_FILE_SUFFIXES = YAML_SUFFIXES | SOURCE_SUFFIXES | HEADER_SUFFIXES

# ``sdkconfig.defaults.board`` and ``Kconfig.projbuild`` are recognised by
# basename rather than suffix (both filenames are fixed by ESP-IDF / bmgr
# conventions). They are NOT auto-discovered — the manifest must list them
# explicitly, just like every other fragment, so users get a single rule
# ("apply: is the only truth").

KIND_YAML = 'yaml'
KIND_SOURCE = 'source'
KIND_HEADER = 'header'
KIND_SDKCONFIG_BOARD = 'sdkconfig_board'
KIND_KCONFIG_PROJBUILD = 'kconfig_projbuild'


class AmendError(Exception):
    """Raised when the amend manifest or any of its items is malformed."""


@dataclass
class AmendFragment:
    """One resolved ``apply`` item from ``board_amend.yaml``."""

    kind: str                 # one of KIND_YAML / KIND_SOURCE / KIND_HEADER / KIND_DIR
    resolved_path: Path       # absolute, normalized
    raw_item: str             # original manifest string, used in error messages
    item_index: int           # zero-based index inside the ``apply`` list

    # Pre-loaded YAML data for KIND_YAML fragments (populated during build_amend_plan).
    yaml_data: Optional[Dict[str, Any]] = None


@dataclass
class AmendPlan:
    """Fully resolved amend plan ready to drive generation."""

    amend_dir: Path                              # absolute path to the amend directory
    manifest_path: Path                          # absolute path to board_amend.yaml
    fragments: List[AmendFragment] = field(default_factory=list)
    description: str = ''                        # manifest-level documentation
    version: str = ''                            # manifest schema version string

    # ------------------------------------------------------------------ helpers
    def yaml_fragments(self) -> List[AmendFragment]:
        return [f for f in self.fragments if f.kind == KIND_YAML]

    def source_fragments(self) -> List[AmendFragment]:
        return [f for f in self.fragments if f.kind == KIND_SOURCE]

    def header_fragments(self) -> List[AmendFragment]:
        return [f for f in self.fragments if f.kind == KIND_HEADER]

    # ----------------------------------- sdkconfig / Kconfig fragment views
    def sdkconfig_sources(self) -> List[Tuple[str, Path]]:
        """Return ``(label, path)`` pairs in manifest order.

        Every fragment whose basename equals ``sdkconfig.defaults.board`` is
        included. Order in the returned list is the order they appear in
        ``apply:``; later items override earlier ones via the
        ``BMGR_CONFIG_OVERRIDE`` marker in the generated
        ``board_manager.defaults`` file.
        """
        sources: List[Tuple[str, Path]] = []
        for frag in self.fragments:
            if frag.kind != KIND_SDKCONFIG_BOARD:
                continue
            label = f'Amend sdkconfig defaults (apply[{frag.item_index}] = {frag.raw_item})'
            sources.append((label, frag.resolved_path))
        return sources

    def kconfig_sources(self) -> List[Tuple[str, Path]]:
        """Return ``(label, path)`` pairs in manifest order (plain text append).

        Labels are kept short because the consumer template wraps them as
        ``# --- {label} Kconfig.projbuild: {path} ---``.
        """
        sources: List[Tuple[str, Path]] = []
        for frag in self.fragments:
            if frag.kind != KIND_KCONFIG_PROJBUILD:
                continue
            label = f'Amend Kconfig (apply[{frag.item_index}] = {frag.raw_item})'
            sources.append((label, frag.resolved_path))
        return sources

    def source_files(self) -> List[Path]:
        return [f.resolved_path for f in self.source_fragments()]

    def include_dirs(self) -> List[Path]:
        """De-duplicated INCLUDE_DIRS for the generated component.

        Derived from each source / header fragment's parent directory, plus
        the amend directory itself. No directory items are inspected because
        the manifest no longer supports them (every file must be listed).
        """
        ordered: List[Path] = []
        seen = set()

        def _add(path: Path) -> None:
            key = str(path.resolve())
            if key not in seen:
                seen.add(key)
                ordered.append(path)

        _add(self.amend_dir)
        for frag in self.fragments:
            if frag.kind in (KIND_SOURCE, KIND_HEADER):
                _add(frag.resolved_path.parent)
        return ordered


# ---------------------------------------------------------------------------
# Manifest loading & validation
# ---------------------------------------------------------------------------

def _classify_path(resolved_path: Path, *, raw_item: str, item_index: int) -> str:
    """Return the AmendFragment kind for ``resolved_path``.

    The manifest only accepts **files** — each fragment must point at a
    concrete file. Directory items are rejected so the rule stays uniform:
    ``apply:`` is the single source of truth, nothing is auto-discovered.

    File classification is two-tiered:

    1. by **basename** for the two filenames whose semantics are fixed by
       ESP-IDF / bmgr (``sdkconfig.defaults.board`` and ``Kconfig.projbuild``)
    2. by **suffix** for everything else (YAML / source / header)

    Unknown suffixes or directory items raise ``AmendError``.
    """
    if resolved_path.is_dir():
        raise AmendError(
            f'Amend item (apply[{item_index}] = {raw_item!r}) is a directory. '
            f'Directory items are not supported — list each file explicitly. '
            f'For example, write "{raw_item}/sdkconfig.defaults.board" and '
            f'"{raw_item}/Kconfig.projbuild" instead of "{raw_item}".'
        )

    if not resolved_path.is_file():
        # Defensive: caller is expected to have checked existence already.
        raise AmendError(
            f'Amend item not found (apply[{item_index}] = {raw_item!r}): {resolved_path}'
        )

    basename = resolved_path.name
    if basename == SDKCONFIG_DEFAULTS_BOARD_FILE:
        return KIND_SDKCONFIG_BOARD
    if basename == KCONFIG_PROJBUILD_FILE:
        return KIND_KCONFIG_PROJBUILD

    suffix = resolved_path.suffix.lower()
    if suffix in YAML_SUFFIXES:
        return KIND_YAML
    if suffix in SOURCE_SUFFIXES:
        return KIND_SOURCE
    if suffix in HEADER_SUFFIXES:
        return KIND_HEADER

    supported = sorted({*YAML_SUFFIXES, *SOURCE_SUFFIXES, *HEADER_SUFFIXES})
    raise AmendError(
        f'Amend item (apply[{item_index}] = {raw_item!r}) has unsupported suffix '
        f'{suffix!r}. Supported suffixes: {", ".join(supported)}. '
        f'Or list the file by its fixed basename: '
        f'{SDKCONFIG_DEFAULTS_BOARD_FILE!r} or {KCONFIG_PROJBUILD_FILE!r}.'
    )


def _resolve_item_path(raw_item: str, amend_dir: Path) -> Path:
    """Resolve a manifest item string to an absolute, normalized path.

    Relative paths are resolved against the amend directory; absolute paths are
    accepted as-is. ``~`` is expanded. ``..`` is permitted and the result may
    leave the amend directory entirely.
    """
    candidate = Path(raw_item).expanduser()
    if not candidate.is_absolute():
        candidate = amend_dir / candidate
    return candidate.resolve(strict=False)


def _load_manifest_data(manifest_path: Path) -> Dict[str, Any]:
    if not manifest_path.is_file():
        raise AmendError(f'Missing manifest: {manifest_path}')
    try:
        data = load_board_yaml_file(manifest_path)
    except BoardConfigYamlError as exc:
        raise AmendError(f'Failed to parse amend manifest {manifest_path}: {exc}') from exc
    if not isinstance(data, dict):
        raise AmendError(
            f'Amend manifest {manifest_path} must be a YAML mapping at the top level'
        )
    return data


def _validate_yaml_fragment_data(
    fragment_data: Optional[Dict[str, Any]],
    *,
    source: Path,
    item_index: int,
    raw_item: str,
) -> Dict[str, Any]:
    """Ensure a YAML fragment is a mapping with devices/peripherals at the top level."""
    if fragment_data is None:
        fragment_data = {}
    if not isinstance(fragment_data, dict):
        raise AmendError(
            f'YAML fragment {source} (apply[{item_index}] = {raw_item!r}) '
            f'must be a YAML mapping at the top level, got {type(fragment_data).__name__}'
        )
    if 'devices' not in fragment_data and 'peripherals' not in fragment_data:
        raise AmendError(
            f'YAML fragment {source} (apply[{item_index}] = {raw_item!r}) '
            "must define top-level 'devices' or 'peripherals'"
        )
    if 'devices' in fragment_data and not isinstance(fragment_data['devices'], list):
        raise AmendError(
            f"YAML fragment {source}: 'devices' must be a list, "
            f"got {type(fragment_data['devices']).__name__}"
        )
    if 'peripherals' in fragment_data and not isinstance(fragment_data['peripherals'], list):
        raise AmendError(
            f"YAML fragment {source}: 'peripherals' must be a list, "
            f"got {type(fragment_data['peripherals']).__name__}"
        )
    return fragment_data


def _load_yaml_fragment(path: Path, *, item_index: int, raw_item: str) -> Dict[str, Any]:
    try:
        data = load_board_yaml_file(path)
    except BoardConfigYamlError as exc:
        raise AmendError(
            f'Failed to parse YAML fragment {path} (apply[{item_index}] = {raw_item!r}): {exc}'
        ) from exc
    return _validate_yaml_fragment_data(
        data, source=path, item_index=item_index, raw_item=raw_item
    )


def _check_portability(
    *,
    raw_item: str,
    resolved_path: Path,
    amend_dir: Path,
    project_root: Optional[Path],
) -> None:
    """Emit debug/info diagnostics for non-portable item references.

    Never raises; this is purely advisory.
    """
    logger = get_logger(__name__)
    try:
        resolved_path.relative_to(amend_dir)
    except ValueError:
        logger.debug(f'   out-of-tree amend item: {raw_item} -> {resolved_path}')
        if project_root is not None:
            try:
                resolved_path.relative_to(project_root)
            except ValueError:
                logger.info(
                    f'ℹ️  absolute / out-of-project amend item is not portable across '
                    f'machines: {raw_item} -> {resolved_path}'
                )


def load_amend_manifest(amend_dir: Path) -> Dict[str, Any]:
    """Load and minimally validate ``board_amend.yaml`` under ``amend_dir``.

    Returns the parsed mapping. Performs structural validation only (top-level
    keys, ``apply`` is a list of non-empty strings). Per-item resolution is
    handled by :func:`build_amend_plan`.
    """
    amend_dir = Path(amend_dir).resolve()
    if not amend_dir.exists():
        raise AmendError(f'Amend path does not exist: {amend_dir}')
    if not amend_dir.is_dir():
        raise AmendError(f'Amend path must be a directory: {amend_dir}')

    manifest_path = amend_dir / MANIFEST_FILENAME
    if not manifest_path.is_file():
        raise AmendError(f'Missing manifest: {manifest_path}')

    data = _load_manifest_data(manifest_path)

    apply_list = data.get('apply')
    if apply_list is None:
        raise AmendError(
            f"Amend manifest {manifest_path} is missing required 'apply' list"
        )
    if not isinstance(apply_list, list):
        raise AmendError(
            f"Amend manifest {manifest_path}: 'apply' must be a list, "
            f'got {type(apply_list).__name__}'
        )

    for idx, item in enumerate(apply_list):
        if not isinstance(item, str):
            raise AmendError(
                f'Amend manifest {manifest_path}: apply[{idx}] must be a string, '
                f'got {type(item).__name__}'
            )
        if not item.strip():
            raise AmendError(
                f'Amend manifest {manifest_path}: apply[{idx}] must be a non-empty string'
            )

    # Optional fields - light validation only.
    version = data.get('version', '')
    if version is not None and not isinstance(version, (str, int, float)):
        raise AmendError(
            f"Amend manifest {manifest_path}: 'version' must be a string"
        )
    description = data.get('description', '')
    if description is not None and not isinstance(description, str):
        raise AmendError(
            f"Amend manifest {manifest_path}: 'description' must be a string"
        )

    return data


def build_amend_plan(
    amend_dir: Path,
    *,
    project_root: Optional[Path] = None,
) -> AmendPlan:
    """Load the manifest and resolve every fragment into an :class:`AmendPlan`.

    All validation that can be done without touching the generation output is
    performed here:

    - amend directory exists and contains ``board_amend.yaml``
    - manifest schema is well-formed
    - every ``apply`` item resolves to an existing file (directories rejected)
    - file-typed items have a supported suffix OR match one of the two fixed
      basenames (``sdkconfig.defaults.board`` / ``Kconfig.projbuild``)
    - YAML fragments parse successfully and contain ``devices`` or ``peripherals``

    Nothing is auto-discovered: any ``sdkconfig.defaults.board`` /
    ``Kconfig.projbuild`` that the user wants applied must appear in
    ``apply:``. Files present in the amend directory but not referenced trigger
    an ``info`` log so authors are aware of dead files.

    Raises:
        AmendError: any validation failure.
    """
    amend_dir = Path(amend_dir).resolve()
    manifest_path = amend_dir / MANIFEST_FILENAME

    manifest_data = load_amend_manifest(amend_dir)

    plan = AmendPlan(
        amend_dir=amend_dir,
        manifest_path=manifest_path,
        version=str(manifest_data.get('version', '') or ''),
        description=str(manifest_data.get('description', '') or ''),
    )

    if plan.version and plan.version != '1.0':
        get_logger(__name__).warning(
            f'⚠️  Unknown amend manifest version {plan.version!r} '
            f'(expected "1.0"); processing as 1.0'
        )

    apply_list: List[str] = list(manifest_data.get('apply') or [])
    project_root_path = Path(project_root).resolve() if project_root else None

    seen_resolved: Dict[str, int] = {}
    for idx, raw_item in enumerate(apply_list):
        resolved = _resolve_item_path(raw_item, amend_dir)
        if not resolved.exists():
            raise AmendError(
                f'Amend item not found (apply[{idx}] = {raw_item!r}): {resolved}'
            )
        kind = _classify_path(resolved, raw_item=raw_item, item_index=idx)

        fragment = AmendFragment(
            kind=kind,
            resolved_path=resolved,
            raw_item=raw_item,
            item_index=idx,
        )
        if kind == KIND_YAML:
            fragment.yaml_data = _load_yaml_fragment(
                resolved, item_index=idx, raw_item=raw_item
            )

        _check_portability(
            raw_item=raw_item,
            resolved_path=resolved,
            amend_dir=amend_dir,
            project_root=project_root_path,
        )

        key = str(resolved)
        if key in seen_resolved:
            get_logger(__name__).debug(
                f'   duplicate amend item: apply[{seen_resolved[key]}] and apply[{idx}] '
                f'both resolve to {resolved}'
            )
        else:
            seen_resolved[key] = idx
        plan.fragments.append(fragment)

    _warn_unreferenced_special_files(plan)

    return plan


def _warn_unreferenced_special_files(plan: AmendPlan) -> None:
    """Emit an info log for top-level sdkconfig/Kconfig files that the
    manifest didn't list. Helps catch "I dropped the file in but forgot to
    add it to apply:" mistakes.
    """
    logger = get_logger(__name__)
    referenced = {
        str(f.resolved_path.resolve())
        for f in plan.fragments
        if f.kind in (KIND_SDKCONFIG_BOARD, KIND_KCONFIG_PROJBUILD)
    }
    candidates = [
        plan.amend_dir / SDKCONFIG_DEFAULTS_BOARD_FILE,
        plan.amend_dir / KCONFIG_PROJBUILD_FILE,
    ]
    for candidate in candidates:
        if not candidate.is_file():
            continue
        if str(candidate.resolve()) in referenced:
            continue
        logger.info(
            f'ℹ️  {candidate.name} present at amend root but not listed in apply:. '
            f"It will be ignored. Add '{candidate.name}' to apply: to apply it."
        )


def merge_amend_plans(
    plans: Iterable[Optional[AmendPlan]],
    *,
    amend_dir: Optional[Path] = None,
    label: str = 'merged',
) -> Optional[AmendPlan]:
    """Concatenate fragments from multiple :class:`AmendPlan`\\ s in order.

    ``None`` entries are filtered out. If the resulting list is empty, ``None``
    is returned (no amend). Otherwise a single :class:`AmendPlan` is produced
    whose ``fragments`` are every input plan's fragments concatenated in order
    and re-indexed with a contiguous ``item_index`` (``0..N-1``). Later plans'
    fragments come after earlier ones, so when applied, later plans override
    earlier ones (matching the overlay priority convention).

    ``amend_dir`` / ``manifest_path`` default to the first plan's values; pass
    ``amend_dir`` to override (it is resolved so ``include_dirs()`` always has a
    concrete directory). ``description`` records the contributing amend dirs.
    """
    real_plans = [p for p in plans if p is not None]
    if not real_plans:
        return None

    first = real_plans[0]
    base_dir = Path(amend_dir).resolve() if amend_dir else first.amend_dir

    merged = AmendPlan(
        amend_dir=base_dir,
        manifest_path=first.manifest_path,
        version=first.version,
        description=f'{label}: ' + ', '.join(str(p.amend_dir) for p in real_plans),
    )

    new_index = 0
    for plan in real_plans:
        for frag in plan.fragments:
            merged.fragments.append(
                AmendFragment(
                    kind=frag.kind,
                    resolved_path=frag.resolved_path,
                    raw_item=frag.raw_item,
                    item_index=new_index,
                    yaml_data=frag.yaml_data,
                )
            )
            new_index += 1

    return merged


# ---------------------------------------------------------------------------
# YAML merging
# ---------------------------------------------------------------------------

def apply_amend_plan_to_yaml(
    base_data: Dict[str, Any],
    plan: Optional[AmendPlan],
    *,
    base_path: Optional[Path] = None,
) -> Dict[str, Any]:
    """Merge ``plan``'s YAML fragments into ``base_data`` in manifest order.

    Each fragment's ``devices`` / ``peripherals`` list is merged into the
    matching list of ``base_data`` (by ``name``) with later items overriding
    earlier ones field-by-field. Items whose ``name`` is not yet present are
    appended.

    Args:
        base_data: parsed mapping from a board YAML (devices or peripherals).
        plan:      amend plan to apply; ``None`` means a no-op.
        base_path: optional source path of ``base_data`` (for diagnostics).

    Returns:
        ``base_data`` mutated in place (also returned for convenience).
    """
    if plan is None:
        return base_data
    if not isinstance(base_data, dict):
        return base_data

    # Local imports to avoid an import cycle: config_utils imports nothing from
    # this module, but it does provide the shared list-merge helpers.
    from .utils.config_utils import (
        apply_device_overrides,
        apply_peripheral_list_overrides,
    )

    is_devices_file = 'devices' in base_data
    is_peripherals_file = 'peripherals' in base_data

    if not is_devices_file and not is_peripherals_file:
        return base_data

    logger = get_logger(__name__)

    for frag in plan.yaml_fragments():
        fragment_data = frag.yaml_data or {}
        applied = False

        if is_devices_file and 'devices' in fragment_data:
            base_data['devices'] = apply_device_overrides(
                base_data.get('devices') or [],
                fragment_data['devices'],
                logger,
            )
            applied = True

        if is_peripherals_file and 'peripherals' in fragment_data:
            base_data['peripherals'] = apply_peripheral_list_overrides(
                base_data.get('peripherals') or [],
                fragment_data['peripherals'],
                logger,
            )
            applied = True

        if applied:
            logger.debug(
                f'   amend applied {frag.resolved_path} '
                f'(apply[{frag.item_index}]) onto {base_path or "<base>"}'
            )

    return base_data


__all__ = [
    'AmendError',
    'AmendFragment',
    'AmendPlan',
    'KIND_YAML',
    'KIND_SOURCE',
    'KIND_HEADER',
    'KIND_SDKCONFIG_BOARD',
    'KIND_KCONFIG_PROJBUILD',
    'MANIFEST_FILENAME',
    'SDKCONFIG_DEFAULTS_BOARD_FILE',
    'KCONFIG_PROJBUILD_FILE',
    'YAML_SUFFIXES',
    'SOURCE_SUFFIXES',
    'HEADER_SUFFIXES',
    'SUPPORTED_FILE_SUFFIXES',
    'build_amend_plan',
    'merge_amend_plans',
    'load_amend_manifest',
    'apply_amend_plan_to_yaml',
]
