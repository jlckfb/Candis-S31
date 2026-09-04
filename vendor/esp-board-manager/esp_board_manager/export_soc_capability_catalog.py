# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

"""Export a static SoC capability catalog for ESP Board Manager and web tools."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import List, Sequence, Tuple

sys.path.insert(0, str(Path(__file__).parent))

from generators.utils.soc_capabilities import SocCapabilityCatalog


def export_soc_capability_catalog(
    requirement_path: Path,
    idf_profiles: Sequence[Tuple[str, Path]],
    chips: Sequence[str],
    output_path: Path,
    strict: bool = False,
) -> None:
    chip_list = list(chips) or _discover_chips(idf_profiles)
    catalog = SocCapabilityCatalog.build(
        requirement_path=Path(requirement_path),
        idf_profiles=[(profile_id, Path(idf_path)) for profile_id, idf_path in idf_profiles],
        chips=chip_list,
    )
    out = Path(output_path)
    out.mkdir(parents=True, exist_ok=True)

    index_data = catalog.to_index_dict()
    diagnostics = list(catalog.diagnostics)
    (out / 'index.json').write_text(
        f'{json.dumps(index_data, indent=2, sort_keys=True)}\n',
        encoding='utf-8',
    )

    for entry in index_data['profiles']:
        profile_data = catalog.to_profile_dict(str(entry['id']))
        diagnostics.extend(catalog.diagnostics)
        (out / str(entry['path'])).write_text(
            f'{json.dumps(profile_data, indent=2, sort_keys=True)}\n',
            encoding='utf-8',
        )

    unique_diagnostics = sorted(set(diagnostics))
    for diagnostic in unique_diagnostics:
        print(f'warning: {diagnostic}', file=sys.stderr)
    if strict and unique_diagnostics:
        raise RuntimeError('strict SoC capability export failed: ' + '; '.join(unique_diagnostics))


def _parse_idf_profile(value: str) -> Tuple[str, Path]:
    if '=' not in value:
        raise argparse.ArgumentTypeError('IDF profile must use <profile-id>=<idf-path>')
    profile_id, path = value.split('=', 1)
    profile_id = profile_id.strip()
    if not profile_id:
        raise argparse.ArgumentTypeError('IDF profile id must not be empty')
    return profile_id, Path(path)


def _discover_chips(idf_profiles: Sequence[Tuple[str, Path]]) -> List[str]:
    chips = set()
    for _, idf_path in idf_profiles:
        soc_root = Path(idf_path) / 'components' / 'soc'
        if not soc_root.is_dir():
            continue
        for chip_dir in soc_root.iterdir():
            if not chip_dir.is_dir():
                continue
            candidates = [
                chip_dir / 'include' / 'soc' / 'soc_caps.h',
                chip_dir / 'include' / 'soc_caps.h',
            ]
            if any(path.is_file() for path in candidates):
                if chip_dir.name == 'linux':
                    continue
                chips.add(chip_dir.name)
    return sorted(chips)


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='Export ESP Board Manager SoC capability catalog.')
    default_root = Path(__file__).parent
    parser.add_argument(
        '--requirements',
        type=Path,
        default=default_root / 'private_inc' / 'esp_board_soc_requirements.yml',
        help='Path to esp_board_soc_requirements.yml.',
    )
    parser.add_argument(
        '--idf-profile',
        action='append',
        type=_parse_idf_profile,
        required=True,
        help='IDF profile in <profile-id>=<idf-path> form. May be passed multiple times.',
    )
    parser.add_argument(
        '--chip',
        action='append',
        default=[],
        help='Chip to include. May be passed multiple times. If omitted, chips are discovered from IDF profiles.',
    )
    parser.add_argument(
        '--output',
        type=Path,
        default=default_root / 'private_inc' / 'soc_capability_catalog',
        help='Output catalog directory containing index.json and per-IDF profile JSON files.',
    )
    parser.add_argument(
        '--strict',
        action='store_true',
        help='Fail when generated profile diagnostics indicate dropped or inconsistent facts.',
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(list(argv or sys.argv[1:]))
    export_soc_capability_catalog(
        requirement_path=args.requirements,
        idf_profiles=args.idf_profile,
        chips=args.chip,
        output_path=args.output,
        strict=args.strict,
    )
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
