# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

"""Pure BMGR static SoC capability validator."""

from __future__ import annotations

import json
import re
from collections.abc import Mapping as MappingABC
from dataclasses import dataclass, field
from typing import Any, Dict, List, Mapping, Optional, Sequence, Set

from .soc_capabilities import SocCapabilityCatalog, SocChipProfile


@dataclass(frozen=True)
class SocValidationField:
    path: List[str]
    value: Any


@dataclass(frozen=True)
class SocValidationInstance:
    instance_id: str
    kind: str
    type: str
    selectors: Dict[str, Any] = field(default_factory=dict)
    fields: List[SocValidationField] = field(default_factory=list)


@dataclass(frozen=True)
class SocValidationIssue:
    code: str
    path: List[Any]
    message: str
    chip: str
    capability: Optional[str] = None
    limit_key: Optional[str] = None
    limit: Optional[int] = None
    actual: Optional[int] = None


@dataclass(frozen=True)
class ResolvedFieldValue:
    path: List[Any]
    value: Any


def validate_soc_capabilities(
    catalog: SocCapabilityCatalog,
    chip: str,
    instances: Sequence[SocValidationInstance],
) -> List[SocValidationIssue]:
    chip_name = _normalize_chip(chip)
    try:
        chip_caps = catalog.chip(chip_name)
    except KeyError:
        return [
            SocValidationIssue(
                code='SOC_CHIP_UNSUPPORTED',
                path=['chip'],
                message=f'{chip_name} is not present in the selected SoC capability catalog.',
                chip=chip_name,
            ),
        ]

    issues: List[SocValidationIssue] = []
    issues.extend(_check_capabilities(catalog, chip_caps, chip_name, instances))
    issues.extend(_check_special_hardware_limits(chip_caps, chip_name, instances))
    issues.extend(_check_hardware_limits(catalog, chip_caps, chip_name, instances))
    return issues


def build_soc_validation_instance(
    kind: str,
    item: Any,
    instance_id: Optional[str] = None,
) -> SocValidationInstance:
    """Build one validator input from a BMGR board YAML record."""

    resolved_id = instance_id if instance_id is not None else _item_value(item, 'name')
    selectors: Dict[str, Any] = {}
    for selector in ('sub_type', 'role', 'format'):
        value = _item_value(item, selector)
        if value is not None:
            selectors[selector] = value

    config = _item_value(item, 'config')
    fields: List[SocValidationField] = []
    if isinstance(config, MappingABC):
        _collect_config_fields(config, [], fields)

    return SocValidationInstance(
        instance_id=str(resolved_id or ''),
        kind=str(kind),
        type=str(_item_value(item, 'type') or ''),
        selectors=selectors,
        fields=fields,
    )


def _check_capabilities(
    catalog: SocCapabilityCatalog,
    chip_caps: SocChipProfile,
    chip: str,
    instances: Sequence[SocValidationInstance],
) -> List[SocValidationIssue]:
    issues: List[SocValidationIssue] = []
    for instance in instances:
        keys = _resolve_capability_keys(catalog, instance)
        if not keys:
            continue
        for key in keys:
            if chip_caps.supports(key):
                continue
            issues.append(
                SocValidationIssue(
                    code='SOC_CAPABILITY_UNSUPPORTED',
                    path=_locator(instance),
                    message=f'{chip} does not support capability {key}.',
                    chip=chip,
                    capability=key,
                ),
            )
    return issues


def _check_hardware_limits(
    catalog: SocCapabilityCatalog,
    chip_caps: SocChipProfile,
    chip: str,
    instances: Sequence[SocValidationInstance],
) -> List[SocValidationIssue]:
    issues: List[SocValidationIssue] = []
    for key, spec in catalog._hardware_limit_defs().items():
        if key in _SPECIAL_HARDWARE_LIMIT_KEYS:
            continue
        limit = chip_caps.hardware_limit(key)
        if limit is None:
            continue

        instance_count = 0
        count_group: Optional[Mapping[str, Any]] = None
        for applies in spec.get('appliesTo', []):
            check = str(applies.get('check', 'value'))
            if check == 'instanceCount':
                count_group = {'kind': applies.get('kind'), 'type': applies.get('type')}
                for instance in instances:
                    if _applies_matches(applies, instance):
                        instance_count += 1
                continue

            path = applies.get('path')
            if not isinstance(path, list):
                continue
            for instance in instances:
                if not _applies_matches(applies, instance):
                    continue
                resolved_values = _resolve_values_for_check(instance, [str(part) for part in path], check)
                if not resolved_values:
                    continue
                compare = str(applies.get('compare', 'le'))
                for resolved in resolved_values:
                    actual = _field_actual_value(resolved.value, check)
                    if actual is None or not _violates_compare(actual, limit, compare):
                        continue
                    issues.append(
                        SocValidationIssue(
                            code='SOC_NUMBER_LIMIT_EXCEEDED',
                            path=[*_locator(instance), *resolved.path],
                            message=f'{key} is {actual}, exceeding {limit} on {chip}.',
                            chip=chip,
                            limit_key=key,
                            limit=limit,
                            actual=actual,
                        ),
                    )

        if count_group and instance_count > limit:
            issues.append(
                SocValidationIssue(
                    code='SOC_NUMBER_LIMIT_EXCEEDED',
                    path=[f"{count_group['kind']}s", count_group['type']],
                    message=f'{key} instance count is {instance_count}, exceeding {limit} on {chip}.',
                    chip=chip,
                    limit_key=key,
                    limit=limit,
                    actual=instance_count,
                ),
            )
    return issues


_SPECIAL_HARDWARE_LIMIT_KEYS = {
    'i2c.instance_count',
    'i2c.hp_instance_count',
    'i2c.lp_instance_count',
    'i2s.instance_count',
}


@dataclass(frozen=True)
class I2CPortResolution:
    family: str
    index: Optional[int]
    path: List[Any]
    value: Any


def _check_special_hardware_limits(
    chip_caps: SocChipProfile,
    chip: str,
    instances: Sequence[SocValidationInstance],
) -> List[SocValidationIssue]:
    issues: List[SocValidationIssue] = []
    issues.extend(validate_i2s_port_direction_duplicates(chip, instances))
    issues.extend(_check_i2s_instance_count(chip_caps, chip, instances))
    issues.extend(_check_i2c_instance_counts(chip_caps, chip, instances))
    return issues


def validate_i2s_port_direction_duplicates(
    chip: str,
    instances: Sequence[SocValidationInstance],
) -> List[SocValidationIssue]:
    seen: Dict[tuple, SocValidationInstance] = {}
    issues: List[SocValidationIssue] = []

    for instance in instances:
        if instance.kind != 'peripheral' or instance.type != 'i2s':
            continue
        direction = _i2s_direction_key(instance)
        if direction is None:
            continue
        key = (_i2s_port_key(instance), direction)
        previous = seen.get(key)
        if previous is None:
            seen[key] = instance
            continue
        issues.append(
            SocValidationIssue(
                code='SOC_DUPLICATE_INSTANCE',
                path=_locator(instance),
                message=(
                    f'I2S port {key[0]} and direction {direction} are already used by '
                    f"peripheral '{previous.instance_id}'."
                ),
                chip=chip,
            ),
        )
    return issues


def _check_i2s_instance_count(
    chip_caps: SocChipProfile,
    chip: str,
    instances: Sequence[SocValidationInstance],
) -> List[SocValidationIssue]:
    limit = chip_caps.hardware_limit('i2s.instance_count')
    if limit is None:
        return []

    ports = {
        _i2s_port_key(instance)
        for instance in instances
        if instance.kind == 'peripheral' and instance.type == 'i2s'
    }
    actual = len(ports)
    if actual <= limit:
        return []
    return [
        SocValidationIssue(
            code='SOC_NUMBER_LIMIT_EXCEEDED',
            path=['peripherals', 'i2s'],
            message=f'i2s.instance_count instance count is {actual}, exceeding {limit} on {chip}.',
            chip=chip,
            limit_key='i2s.instance_count',
            limit=limit,
            actual=actual,
        )
    ]


def _i2s_port_key(instance: SocValidationInstance) -> str:
    resolved_values = _resolve_field_values(instance, ['port'])
    value = resolved_values[0].value if len(resolved_values) == 1 else 0
    index = _value_index(value)
    if index is not None:
        return str(index)
    return str(value).strip()


def _i2s_direction_key(instance: SocValidationInstance) -> Optional[str]:
    values = _resolve_field_values(instance, ['direction'])
    if len(values) == 1:
        value = str(values[0].value).strip().upper()
        if value == 'I2S_DIR_TX':
            return 'tx'
        if value == 'I2S_DIR_RX':
            return 'rx'

    formats = instance.selectors.get('format')
    if isinstance(formats, list):
        if len(formats) != 1:
            return None
        formats = formats[0]
    format_text = str(formats or '').strip().lower()
    if format_text.endswith('-out'):
        return 'tx'
    if format_text.endswith('-in'):
        return 'rx'
    return None


def _check_i2c_instance_counts(
    chip_caps: SocChipProfile,
    chip: str,
    instances: Sequence[SocValidationInstance],
) -> List[SocValidationIssue]:
    i2c_instances = [
        instance for instance in instances if instance.kind == 'peripheral' and instance.type == 'i2c'
    ]
    if not i2c_instances:
        return []

    total_limit = chip_caps.hardware_limit('i2c.instance_count')
    lp_limit = chip_caps.hardware_limit('i2c.lp_instance_count')
    hp_limit = _i2c_hp_limit(chip_caps, total_limit, lp_limit)
    issues: List[SocValidationIssue] = []

    total_exceeded = total_limit is not None and len(i2c_instances) > total_limit
    if total_exceeded:
        issues.append(
            SocValidationIssue(
                code='SOC_NUMBER_LIMIT_EXCEEDED',
                path=['peripherals', 'i2c'],
                message=f'i2c.instance_count instance count is {len(i2c_instances)}, exceeding {total_limit} on {chip}.',
                chip=chip,
                limit_key='i2c.instance_count',
                limit=total_limit,
                actual=len(i2c_instances),
            )
        )

    hp_actual = 0
    lp_actual = 0
    for instance in i2c_instances:
        resolved = _resolve_i2c_port(instance, total_limit, hp_limit, lp_limit)
        if resolved.family == 'lp':
            lp_actual += 1
            continue
        if resolved.family == 'hp':
            hp_actual += 1
            continue
        if resolved.family != 'invalid':
            continue
        limit_key = _i2c_invalid_port_limit_key(resolved.value, hp_limit)
        limit = chip_caps.hardware_limit(limit_key)
        if limit is None and limit_key == 'i2c.hp_instance_count':
            limit = hp_limit
        if limit is None and limit_key == 'i2c.instance_count' and total_limit is None and hp_limit is not None:
            limit_key = 'i2c.hp_instance_count'
            limit = hp_limit
        if limit is None:
            limit_key = 'i2c.instance_count'
            limit = total_limit
        issues.append(
            SocValidationIssue(
                code='SOC_NUMBER_LIMIT_EXCEEDED',
                path=[*_locator(instance), *resolved.path],
                message=f'{limit_key} port value {resolved.value} exceeds {limit} on {chip}.',
                chip=chip,
                limit_key=limit_key,
                limit=limit,
                actual=resolved.index,
            )
        )

    if not total_exceeded and hp_limit is not None and hp_actual > hp_limit:
        issues.append(
            SocValidationIssue(
                code='SOC_NUMBER_LIMIT_EXCEEDED',
                path=['peripherals', 'i2c'],
                message=f'i2c.hp_instance_count instance count is {hp_actual}, exceeding {hp_limit} on {chip}.',
                chip=chip,
                limit_key='i2c.hp_instance_count',
                limit=hp_limit,
                actual=hp_actual,
            )
        )

    if not total_exceeded and lp_limit is not None and lp_actual > lp_limit:
        issues.append(
            SocValidationIssue(
                code='SOC_NUMBER_LIMIT_EXCEEDED',
                path=['peripherals', 'i2c'],
                message=f'i2c.lp_instance_count instance count is {lp_actual}, exceeding {lp_limit} on {chip}.',
                chip=chip,
                limit_key='i2c.lp_instance_count',
                limit=lp_limit,
                actual=lp_actual,
            )
        )
    return issues


def _i2c_hp_limit(
    chip_caps: SocChipProfile,
    total_limit: Optional[int],
    lp_limit: Optional[int],
) -> Optional[int]:
    hp_limit = chip_caps.hardware_limit('i2c.hp_instance_count')
    if hp_limit is not None:
        return hp_limit
    if total_limit is not None and lp_limit is not None:
        return max(total_limit - lp_limit, 0)
    return None


def _resolve_i2c_port(
    instance: SocValidationInstance,
    total_limit: Optional[int],
    hp_limit: Optional[int],
    lp_limit: Optional[int],
) -> I2CPortResolution:
    resolved_values = _resolve_field_values(instance, ['port'])
    if len(resolved_values) != 1:
        return I2CPortResolution('hp', None, ['port'], -1)

    resolved = resolved_values[0]
    value = resolved.value
    text = str(value).strip()
    if text in {'', '-1'}:
        return I2CPortResolution('hp', -1, resolved.path, value)

    lp_match = re.fullmatch(r'LP_I2C_NUM_(\d+)', text)
    if lp_match:
        index = int(lp_match.group(1))
        if lp_limit is not None and index >= lp_limit:
            return I2CPortResolution('invalid', index, resolved.path, value)
        return I2CPortResolution('lp', index, resolved.path, value)

    hp_match = re.fullmatch(r'I2C_NUM_(\d+)', text)
    if hp_match:
        index = int(hp_match.group(1))
        if hp_limit is not None and index >= hp_limit:
            return I2CPortResolution('invalid', index, resolved.path, value)
        return I2CPortResolution('hp', index, resolved.path, value)

    index = _value_index(value)
    if index is None:
        return I2CPortResolution('hp', None, resolved.path, value)
    if index < 0:
        return I2CPortResolution('hp', index, resolved.path, value)
    if hp_limit is not None and index < hp_limit:
        return I2CPortResolution('hp', index, resolved.path, value)
    if total_limit is not None and index < total_limit:
        return I2CPortResolution('lp', index - (hp_limit or 0), resolved.path, value)
    if hp_limit is None:
        return I2CPortResolution('hp', index, resolved.path, value)
    return I2CPortResolution('invalid', index, resolved.path, value)


def _i2c_invalid_port_limit_key(value: Any, hp_limit: Optional[int]) -> str:
    if hp_limit is not None and re.fullmatch(r'I2C_NUM_\d+', str(value).strip()):
        return 'i2c.hp_instance_count'
    if re.fullmatch(r'LP_I2C_NUM_\d+', str(value).strip()):
        return 'i2c.lp_instance_count'
    return 'i2c.instance_count'


def _field_actual_value(value: Any, check: str) -> Optional[int]:
    if check == 'arrayLength':
        return _array_length(value)
    return _value_index(value)


def _value_index(value: Any) -> Optional[int]:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    text = str(value).strip()
    if re.fullmatch(r'\d+', text):
        return int(text)
    match = re.search(r'_(\d+)$', text)
    return int(match.group(1)) if match else None


def _violates_compare(actual: int, limit: int, compare: str) -> bool:
    normalized = str(compare or 'le').strip().lower()
    if normalized == 'lt':
        return actual >= limit
    if normalized == 'ge':
        return actual < limit
    if normalized == 'gt':
        return actual <= limit
    if normalized == 'eq':
        return actual != limit
    if normalized == 'ne':
        return actual == limit
    return actual > limit


def _resolve_capability_key(catalog: SocCapabilityCatalog, instance: SocValidationInstance) -> Optional[str]:
    keys = _resolve_capability_keys(catalog, instance)
    return keys[0] if keys else None


def _resolve_capability_keys(catalog: SocCapabilityCatalog, instance: SocValidationInstance) -> List[str]:
    matches = []
    for key, raw_def in catalog._capability_defs().items():
        if raw_def.get('kind') != instance.kind or raw_def.get('type') != instance.type:
            continue
        score = _match_dimensions(raw_def, {'kind', 'type'}, instance)
        if score is None:
            continue
        matches.append((score, key))
    return [key for _score, key in sorted(matches, key=lambda item: (-item[0], item[1]))]


def _applies_matches(applies: Mapping[str, Any], instance: SocValidationInstance) -> bool:
    if applies.get('kind') != instance.kind or applies.get('type') != instance.type:
        return False
    return _match_dimensions(applies, {'kind', 'type', 'path', 'check', 'compare'}, instance) is not None


def _match_dimensions(
    entry: Mapping[str, Any],
    reserved: Set[str],
    instance: SocValidationInstance,
) -> Optional[int]:
    matched = 0
    for key, value in entry.items():
        if key in reserved:
            continue
        allowed = _as_string_list(value)
        if allowed is None:
            continue
        if not _selector_matches(instance.selectors.get(key), allowed):
            return None
        matched += 1
    return matched


def _find_field(instance: SocValidationInstance, path: Sequence[str]) -> Optional[SocValidationField]:
    wanted = list(path)
    for field_item in instance.fields:
        if field_item.path == wanted:
            return field_item
    return None


def _resolve_values_for_check(
    instance: SocValidationInstance,
    path: Sequence[str],
    check: str,
) -> List[ResolvedFieldValue]:
    if check == 'arrayLength':
        field_item = _find_field(instance, path)
        if field_item is None:
            return []
        return [ResolvedFieldValue(path=list(field_item.path), value=field_item.value)]
    return _resolve_field_values(instance, path)


def _resolve_field_values(instance: SocValidationInstance, path: Sequence[str]) -> List[ResolvedFieldValue]:
    if not path:
        return []
    fields_by_path = {tuple(field_item.path): field_item.value for field_item in instance.fields}
    wanted = [str(part) for part in path]
    direct_key = tuple(wanted)
    if direct_key in fields_by_path:
        return _expand_resolved_value(list(wanted), fields_by_path[direct_key], is_terminal=True)
    return _resolve_from_collected_fields(fields_by_path, wanted)


def _resolve_from_collected_fields(
    fields_by_path: Mapping[tuple, Any],
    wanted: Sequence[str],
) -> List[ResolvedFieldValue]:
    if not wanted:
        return []
    first_key = (wanted[0],)
    if first_key not in fields_by_path:
        return []
    root = fields_by_path[first_key]
    if len(wanted) == 1:
        return _expand_resolved_value([wanted[0]], root, is_terminal=True)
    return _resolve_path_from_value(root, [wanted[0]], list(wanted[1:]))


def _resolve_path_from_value(value: Any, actual_path: List[Any], remaining: Sequence[str]) -> List[ResolvedFieldValue]:
    if not remaining:
        return _expand_resolved_value(actual_path, value, is_terminal=True)
    if isinstance(value, list):
        resolved = []
        for index, item in enumerate(value):
            resolved.extend(_resolve_path_from_value(item, [*actual_path, index], remaining))
        return resolved
    if isinstance(value, MappingABC):
        key = remaining[0]
        if key not in value:
            return []
        return _resolve_path_from_value(value[key], [*actual_path, key], remaining[1:])
    return []


def _expand_resolved_value(actual_path: List[Any], value: Any, is_terminal: bool) -> List[ResolvedFieldValue]:
    if is_terminal and isinstance(value, list):
        return [
            ResolvedFieldValue(path=[*actual_path, index], value=item)
            for index, item in enumerate(value)
            if not isinstance(item, MappingABC)
        ]
    return [ResolvedFieldValue(path=actual_path, value=value)]


def _array_length(value: Any) -> int:
    if isinstance(value, list):
        return len(value)
    parsed = _json_loads(value)
    return len(parsed) if isinstance(parsed, list) else 0


def _json_loads(value: Any) -> Any:
    try:
        return json.loads(value)
    except (TypeError, ValueError):
        return None


def _selector_matches(value: Any, allowed: Sequence[str]) -> bool:
    if value is None:
        return False
    values = value if isinstance(value, list) else [value]
    return any(str(item) in allowed for item in values)


def _item_value(item: Any, key: str) -> Any:
    if isinstance(item, MappingABC):
        return item.get(key)
    return getattr(item, key, None)


def _collect_config_fields(config: Mapping[str, Any], prefix: List[str], fields: List[SocValidationField]) -> None:
    for key, value in config.items():
        path = [*prefix, str(key)]
        if isinstance(value, MappingABC):
            fields.append(SocValidationField(path=path, value=value))
            _collect_config_fields(value, path, fields)
            continue
        fields.append(SocValidationField(path=path, value=value))


def _as_string_list(value: Any) -> Optional[List[str]]:
    if not isinstance(value, list):
        return None
    if not all(isinstance(item, str) for item in value):
        return None
    return list(value)


def _locator(instance: SocValidationInstance) -> List[Any]:
    return [f'{instance.kind}s', instance.instance_id]


def _normalize_chip(chip: str) -> str:
    return str(chip).strip().lower().replace('-', '')
