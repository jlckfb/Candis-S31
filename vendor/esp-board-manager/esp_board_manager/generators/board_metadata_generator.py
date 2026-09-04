#!/usr/bin/env python3
"""
# SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.
"""

"""Board metadata generator."""

import logging
from itertools import combinations
from pathlib import Path
from typing import Any, Dict, Iterable, Iterator, List, Optional, Tuple

import yaml

from generators.utils.logger import get_logger
from generators.utils.soc_capability_query import current_soc, current_soc_chip


class InlineList(list):
    """YAML sequence that should be emitted in flow style."""


class MetadataDumper(yaml.SafeDumper):
    """Custom dumper for board metadata."""


def _represent_inline_list(dumper, data):
    return dumper.represent_sequence('tag:yaml.org,2002:seq', data, flow_style=True)


MetadataDumper.add_representer(InlineList, _represent_inline_list)
logger = get_logger(__name__)


def _format_family(format_name: Optional[str]) -> Optional[str]:
    if not format_name:
        return None
    return str(format_name).split('-', 1)[0]


def _normalize_scalar_io(value: Any) -> Optional[Any]:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        if value < 0:
            return None
        return value
    if isinstance(value, str):
        value = value.strip()
        if value.startswith('GPIO_NUM_') or value.startswith('IO_EXPANDER_PIN_NUM_'):
            if value in ('GPIO_NUM_NC', 'IO_EXPANDER_PIN_NUM_NC'):
                return None
            return value
    return None


def _normalize_io_value(value: Any) -> Optional[Any]:
    scalar_value = _normalize_scalar_io(value)
    if scalar_value is not None:
        return scalar_value

    if isinstance(value, list):
        normalized_list = []
        for item in value:
            normalized_item = _normalize_scalar_io(item)
            if normalized_item is not None:
                normalized_list.append(normalized_item)
        if normalized_list:
            return normalized_list

    return None


def _stable_value_key(value: Any) -> Any:
    if isinstance(value, list):
        return tuple(value)
    return value


def _prune_empty(value: Any) -> Any:
    if isinstance(value, dict):
        pruned = {}
        for key, item in value.items():
            pruned_item = _prune_empty(item)
            if pruned_item in ({}, [], None):
                continue
            pruned[key] = pruned_item
        return pruned

    if isinstance(value, list):
        kept_items = [item for item in value if item not in ({}, [], None)]
        if isinstance(value, InlineList):
            return InlineList(kept_items)
        return kept_items

    return value


def _inline_io_lists(value: Any) -> Any:
    if isinstance(value, dict):
        return {key: _inline_io_lists(item) for key, item in value.items()}

    if isinstance(value, list):
        return InlineList(_inline_io_lists(item) for item in value)

    return value


def _optional_peripheral_role(role: Optional[str]) -> Optional[str]:
    if role in (None, '', 'none'):
        return None
    return role


def _optional_peripheral_format(format_name: Optional[str]) -> Optional[str]:
    if format_name in (None, ''):
        return None
    return format_name


def _gpio_candidate(value: Any) -> Optional[Any]:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        if value < 0:
            return None
        return value
    if isinstance(value, str):
        value = value.strip()
        if value in ('', '-1', 'GPIO_NUM_NC', 'IO_EXPANDER_PIN_NUM_NC'):
            return None
        if value.startswith('IO_EXPANDER_PIN_NUM_'):
            return None
        if value.startswith('GPIO_NUM_'):
            try:
                pin = int(value.removeprefix('GPIO_NUM_'))
            except ValueError:
                return None
            if pin < 0:
                return None
            return pin
    return None


def _display_gpio_pin(pin: Any) -> Any:
    if isinstance(pin, str):
        raw = pin.strip()
        if raw.startswith('GPIO_NUM_'):
            try:
                return int(raw.removeprefix('GPIO_NUM_'))
            except ValueError:
                return pin
    return pin


def _is_io_expander_field(field: str) -> bool:
    field_name = str(field or '').lower()
    return 'expander' in field_name or field_name.startswith('io_expander')


def _iter_metadata_io_values(io_metadata: Any) -> Iterator[Tuple[str, Any]]:
    def walk(node: Any, field: str) -> Iterator[Tuple[str, Any]]:
        if isinstance(node, dict):
            for key, value in node.items():
                yield from walk(value, str(key))
            return

        if isinstance(node, list):
            for item in node:
                yield from walk(item, field)
            return

        if _is_io_expander_field(field):
            return

        pin = _gpio_candidate(node)
        if pin is not None:
            yield field, pin

    yield from walk(io_metadata, '')


def _format_metadata_yaml(yaml_text: str) -> str:
    lines = yaml_text.splitlines()
    formatted = []
    section_headers = ('devices:', 'peripherals:')

    for index, line in enumerate(lines):
        if (
            formatted
            and line in section_headers
            and formatted[-1] != ''
        ):
            formatted.append('')

        if (
            formatted
            and line.startswith('  ')
            and line.endswith(':')
            and not line.startswith('    ')
            and formatted[-1] != ''
            and formatted[-1] not in section_headers
        ):
            formatted.append('')

        formatted.append(line)

    return '\n'.join(formatted) + '\n'


def _format_io_conflict_message(first: Dict[str, Any], second: Dict[str, Any], pin: Any) -> str:
    return (
        'IO conflict: %s (%s).%s <-> %s (%s).%s on GPIO %s'
        % (
            first.get('owner_name'),
            first.get('owner_type'),
            first.get('io_field'),
            second.get('owner_name'),
            second.get('owner_type'),
            second.get('io_field'),
            _display_gpio_pin(pin),
        )
    )


class BoardMetadataGenerator:
    """Build and write unified board metadata YAML."""

    def __init__(self) -> None:
        self._io_conflict_warnings: List[str] = []

    @property
    def io_conflict_warnings(self) -> List[str]:
        return list(self._io_conflict_warnings)

    def report_io_conflict_warnings(self, log: Optional[logging.Logger] = None) -> int:
        """Emit a grouped IO conflict report. Returns the number of warnings."""
        if not self._io_conflict_warnings:
            return 0

        log = log or logger
        count = len(self._io_conflict_warnings)
        log.warning('⚠️  IO conflict warnings (%d)', count)
        for index, message in enumerate(self._io_conflict_warnings, 1):
            log.warning('  [%d] %s', index, message)
        log.warning('Review board YAML if any entry looks unexpected.')
        return count

    def _normalize_sdmmc_slot(self, slot: Any) -> Any:
        if isinstance(slot, str):
            raw = slot.strip()
            if raw == 'SDMMC_HOST_SLOT_0':
                return 0
            if raw.startswith('SDMMC_HOST_SLOT_'):
                try:
                    return int(raw.removeprefix('SDMMC_HOST_SLOT_'))
                except ValueError:
                    return raw
        return slot

    def _is_sdmmc_slot0_placeholder(self, record: Dict[str, Any]) -> bool:
        artifact = record.get('artifact', {})
        if not isinstance(artifact, dict):
            return False
        if artifact.get('sub_type') != 'sdmmc':
            return False
        if _gpio_candidate(record.get('pin')) != 0:
            return False

        raw = artifact.get('raw', {}) if isinstance(artifact, dict) else {}
        raw_config = raw.get('config', {}) if isinstance(raw, dict) else {}
        sub_config = raw_config.get('sub_config', {}) if isinstance(raw_config, dict) else {}
        slot = sub_config.get('slot')
        if slot is None:
            result = artifact.get('result', {}) if isinstance(artifact, dict) else {}
            struct_init = result.get('struct_init', {}) if isinstance(result, dict) else {}
            sub_cfg = struct_init.get('sub_cfg', {}) if isinstance(struct_init, dict) else {}
            sdmmc_cfg = sub_cfg.get('sdmmc', {}) if isinstance(sub_cfg, dict) else {}
            slot = sdmmc_cfg.get('slot')

        return self._normalize_sdmmc_slot(slot) == 0

    def _is_ignored_duplicate_record(self, record: Dict[str, Any]) -> bool:
        return self._is_sdmmc_slot0_placeholder(record)

    def _duplicate_pair_conflicts(self, first: Dict[str, Any], second: Dict[str, Any]) -> bool:
        first_type = first.get('owner_type')
        second_type = second.get('owner_type')
        if first_type == 'i2s' and second_type == 'i2s':
            from peripherals.periph_i2s.periph_i2s import is_duplicate_io_conflict

            return is_duplicate_io_conflict([first, second])
        return True

    def _order_duplicate_pair(self, first: Dict[str, Any], second: Dict[str, Any]) -> Tuple[Dict[str, Any], Dict[str, Any]]:
        first_type = first.get('owner_type')
        second_type = second.get('owner_type')
        if first_type != 'i2s' and second_type == 'i2s':
            return first, second
        if first_type == 'i2s' and second_type != 'i2s':
            return second, first
        return first, second

    def _record_duplicate_warning(self, first: Dict[str, Any], second: Dict[str, Any], pin: Any) -> None:
        self._io_conflict_warnings.append(_format_io_conflict_message(first, second, pin))

    def _collect_io_records(
        self,
        artifact: Dict[str, Any],
        owner_kind: str,
        io_metadata: Any,
    ) -> List[Dict[str, Any]]:
        records: List[Dict[str, Any]] = []

        def walk(node: Any, field: str) -> None:
            if isinstance(node, dict):
                for key, value in node.items():
                    walk(value, str(key))
                return

            if isinstance(node, list):
                for item in node:
                    walk(item, field)
                return

            if _is_io_expander_field(field):
                return

            pin = _gpio_candidate(node)
            if pin is None:
                return

            record = {
                'pin': pin,
                'owner_name': artifact.get('name'),
                'owner_kind': owner_kind,
                'owner_type': artifact.get('type'),
                'io_field': field,
                'artifact': artifact,
            }
            if self._is_ignored_duplicate_record(record):
                return

            records.append(record)

        walk(io_metadata, '')
        return records

    def _warn_duplicate_io_records(self, io_records: List[Dict[str, Any]]) -> None:
        by_pin: Dict[Any, List[Dict[str, Any]]] = {}
        for record in io_records:
            by_pin.setdefault(record['pin'], []).append(record)

        for pin, group in by_pin.items():
            if len(group) < 2:
                continue

            for first, second in combinations(group, 2):
                if not self._duplicate_pair_conflicts(first, second):
                    continue

                ordered_first, ordered_second = self._order_duplicate_pair(first, second)
                self._record_duplicate_warning(ordered_first, ordered_second, pin)

    def validate_metadata_io(self, metadata: Dict[str, Any]) -> None:
        """Validate extracted metadata IO against the configured SoC catalog."""
        chip_profile = current_soc_chip()
        if chip_profile is None:
            return

        soc = current_soc()
        chip_name = str(metadata.get('chip') or chip_profile.chip)

        for section, display_kind in (('devices', 'device'), ('peripherals', 'peripheral')):
            objects = metadata.get(section, {})
            if not isinstance(objects, dict):
                continue
            for name, object_metadata in objects.items():
                if not isinstance(object_metadata, dict):
                    continue
                io_metadata = object_metadata.get('io')
                if io_metadata in ({}, [], None):
                    continue
                for field, pin in _iter_metadata_io_values(io_metadata):
                    if soc.valid_gpio(pin, direction='any', allow_nc=True, default=True):
                        continue
                    raise ValueError(
                        "Invalid GPIO for %s '%s' io field '%s': pin %s is not valid on chip %s"
                        % (display_kind, name, field, pin, chip_name)
                    )

    def _get_module_io_spec(self, parse_func, component_type: str, kind: str) -> Any:
        prefix = 'DEV' if kind == 'device' else 'PERIPH'
        constant_name = f'{prefix}_{component_type.upper()}_IO_LIST'
        return parse_func.__globals__.get(constant_name)

    def _get_custom_extractor(self, parse_func):
        return parse_func.__globals__.get('extract_metadata')

    def _resolve_io_fields(self, artifact: Dict[str, Any], kind: str) -> List[str]:
        parse_func = artifact.get('parse_func')
        if parse_func is None:
            return []

        io_spec = self._get_module_io_spec(parse_func, artifact.get('type', ''), kind)
        if io_spec is None:
            return []

        if isinstance(io_spec, (list, tuple, set)):
            return list(io_spec)

        if not isinstance(io_spec, dict):
            raise ValueError(
                f'Unsupported IO spec type for {kind} {artifact.get("name")}: '
                f'{type(io_spec).__name__}'
            )

        if kind == 'device':
            sub_type = artifact.get('sub_type')
            selected = io_spec.get(sub_type)
            if selected is None:
                selected = io_spec.get('default', [])
            return list(selected)

        role = artifact.get('role')
        format_name = artifact.get('format')
        format_family = _format_family(format_name)
        for key in (role, format_name, format_family, 'default'):
            if key is None:
                continue
            if key in io_spec:
                return list(io_spec[key])
        return []

    def _collect_matching_io(self, struct_init: Any, target_fields: Iterable[str]) -> Dict[str, Any]:
        targets = set(target_fields)
        if not targets:
            return {}

        matches: Dict[str, List[Any]] = {}

        def record(key: str, value: Any) -> None:
            normalized = _normalize_io_value(value)
            if normalized is None:
                return
            matches.setdefault(key, []).append(normalized)

        def walk(node: Any) -> None:
            if isinstance(node, dict):
                for key, value in node.items():
                    if key in targets:
                        record(key, value)

                    if isinstance(value, list):
                        if value and all(not isinstance(item, (dict, list)) for item in value):
                            for index, item in enumerate(value):
                                synthetic_key = f'{key}_{index}'
                                if synthetic_key in targets:
                                    record(synthetic_key, item)
                    walk(value)
                return

            if isinstance(node, list):
                for item in node:
                    walk(item)

        walk(struct_init)

        resolved: Dict[str, Any] = {}
        for key, values in matches.items():
            unique_values: Dict[Any, Any] = {}
            for value in values:
                unique_values[_stable_value_key(value)] = value

            if len(unique_values) > 1:
                raise ValueError(
                    f"Ambiguous IO field '{key}' produced multiple values in metadata extraction"
                )

            resolved[key] = next(iter(unique_values.values()))

        return resolved

    def _build_device_metadata(
        self,
        artifact: Dict[str, Any],
        context: Dict[str, Any],
    ) -> Tuple[Dict[str, Any], List[Dict[str, Any]]]:
        extractor = self._get_custom_extractor(artifact.get('parse_func'))
        if extractor:
            custom_result = extractor(artifact['name'], artifact.get('raw', {}), artifact['result'], context)
            if not isinstance(custom_result, dict):
                raise ValueError(
                    f"Custom metadata extractor for device '{artifact['name']}' must return a dict"
                )
            io_metadata = _prune_empty(custom_result.get('io', {}))
        else:
            io_fields = self._resolve_io_fields(artifact, kind='device')
            io_metadata = self._collect_matching_io(artifact['result'].get('struct_init', {}), io_fields)
        io_metadata = _inline_io_lists(io_metadata)
        io_records = self._collect_io_records(artifact, 'device', io_metadata)

        return _prune_empty({
            'type': artifact.get('type'),
            'sub_type': artifact.get('sub_type'),
            'peripherals': list(artifact.get('peripherals', [])),
            'dependencies': artifact.get('dependencies', {}) or {},
            'io': io_metadata,
        }), io_records

    def _build_peripheral_metadata(
        self,
        artifact: Dict[str, Any],
        context: Dict[str, Any],
    ) -> Tuple[Dict[str, Any], List[Dict[str, Any]]]:
        extractor = self._get_custom_extractor(artifact.get('parse_func'))
        if extractor:
            custom_result = extractor(artifact['name'], artifact.get('raw', {}), artifact['result'], context)
            if not isinstance(custom_result, dict):
                raise ValueError(
                    f"Custom metadata extractor for peripheral '{artifact['name']}' must return a dict"
                )
            io_metadata = _prune_empty(custom_result.get('io', {}))
        else:
            io_fields = self._resolve_io_fields(artifact, kind='peripheral')
            io_metadata = self._collect_matching_io(artifact['result'].get('struct_init', {}), io_fields)
        io_metadata = _inline_io_lists(io_metadata)
        io_records = self._collect_io_records(artifact, 'peripheral', io_metadata)

        return _prune_empty({
            'type': artifact.get('type'),
            'role': _optional_peripheral_role(artifact.get('role')),
            'format': _optional_peripheral_format(artifact.get('format')),
            'io': io_metadata,
        }), io_records

    def generate_metadata_dict(
        self,
        board_name: str,
        chip_name: str,
        device_artifacts: List[Dict[str, Any]],
        peripheral_artifacts: List[Dict[str, Any]],
    ) -> Dict[str, Any]:
        self._io_conflict_warnings = []

        context = {
            'board': board_name,
            'chip': chip_name,
        }

        devices = {}
        device_io_records: List[Dict[str, Any]] = []
        for artifact in device_artifacts:
            metadata, io_records = self._build_device_metadata(artifact, context)
            devices[artifact['name']] = metadata
            device_io_records.extend(io_records)

        peripherals = {}
        peripheral_io_records: List[Dict[str, Any]] = []
        for artifact in peripheral_artifacts:
            metadata, io_records = self._build_peripheral_metadata(artifact, context)
            peripherals[artifact['name']] = metadata
            peripheral_io_records.extend(io_records)

        self._warn_duplicate_io_records(device_io_records + peripheral_io_records)

        return {
            'version': 1,
            'board': board_name,
            'chip': chip_name,
            'devices': devices,
            'peripherals': peripherals,
        }

    def write_metadata_file(
        self,
        output_path: str,
        board_name: str,
        chip_name: str,
        device_artifacts: List[Dict[str, Any]],
        peripheral_artifacts: List[Dict[str, Any]],
    ) -> Dict[str, Any]:
        metadata = self.generate_metadata_dict(
            board_name=board_name,
            chip_name=chip_name,
            device_artifacts=device_artifacts,
            peripheral_artifacts=peripheral_artifacts,
        )
        self.validate_metadata_io(metadata)

        output = Path(output_path)
        output.parent.mkdir(parents=True, exist_ok=True)
        yaml_text = yaml.dump(
            metadata,
            Dumper=MetadataDumper,
            sort_keys=False,
            allow_unicode=False,
        )
        with output.open('w', encoding='utf-8') as f:
            f.write(_format_metadata_yaml(yaml_text))

        return metadata
