# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

"""Shared ESP-IDF SoC capability catalog helpers."""

from __future__ import annotations

import ast
import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

import yaml

from .adc_channel_map_parser import (
    AdcChannelMap,
    adc_channel_map_from_catalog,
    adc_channel_map_to_catalog,
    parse_adc_channel_header,
)


SCHEMA_VERSION = 3
INDEX_SCHEMA_VERSION = 1


@dataclass(frozen=True)
class GpioMacroSpec:
    """Macro names used to derive GPIO validity."""

    valid_input_mask: str = 'SOC_GPIO_VALID_GPIO_MASK'
    valid_output_mask: str = 'SOC_GPIO_VALID_OUTPUT_GPIO_MASK'
    input_range_max: str = 'SOC_GPIO_IN_RANGE_MAX'
    output_range_max: str = 'SOC_GPIO_OUT_RANGE_MAX'

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> 'GpioMacroSpec':
        return cls(
            valid_input_mask=str(data.get('validInputMask', cls.valid_input_mask)),
            valid_output_mask=str(data.get('validOutputMask', cls.valid_output_mask)),
            input_range_max=str(data.get('inputRangeMax', cls.input_range_max)),
            output_range_max=str(data.get('outputRangeMax', cls.output_range_max)),
        )

    def to_dict(self) -> Dict[str, str]:
        return {
            'validInputMask': self.valid_input_mask,
            'validOutputMask': self.valid_output_mask,
            'inputRangeMax': self.input_range_max,
            'outputRangeMax': self.output_range_max,
        }


@dataclass(frozen=True)
class SocRequirementRule:
    """Macros required by one BMGR capability key."""

    all_of: List[str] = field(default_factory=list)
    any_of: List[str] = field(default_factory=list)

    def evaluate(self, booleans: Mapping[str, bool]) -> bool:
        if self.all_of and not all(booleans.get(macro, False) for macro in self.all_of):
            return False
        if self.any_of and not any(booleans.get(macro, False) for macro in self.any_of):
            return False
        return True

    def to_dict(self) -> Dict[str, List[str]]:
        data: Dict[str, List[str]] = {}
        if self.all_of:
            data['allOf'] = list(self.all_of)
        if self.any_of:
            data['anyOf'] = list(self.any_of)
        return data


def _selector_values(value: Any) -> List[str]:
    return [str(item) for item in value] if isinstance(value, list) else [str(value)]


@dataclass(frozen=True)
class CapabilityMatch:
    """Maps a capability key to the board-YAML selection dimensions that pick it.

    Expressed directly in board-YAML vocabulary: ``type`` plus inline selection
    dimensions such as ``sub_type`` / ``role`` / ``format``. The serialized form
    keeps ``kind`` / ``type`` as reserved keys; every other key is a selection
    dimension whose value is the set of accepted board-YAML values.
    """

    _RESERVED = ('kind', 'type')

    kind: str
    type: str
    selectors: Dict[str, List[str]] = field(default_factory=dict)

    @classmethod
    def from_yaml(cls, kind: str, data: Mapping[str, Any]) -> 'CapabilityMatch':
        selectors: Dict[str, List[str]] = {}
        for key, value in dict(data).items():
            if key in cls._RESERVED:
                continue
            selectors[str(key)] = _selector_values(value)
        return cls(kind=str(kind), type=str(data.get('type', '')).strip(), selectors=selectors)

    def to_dict(self) -> Dict[str, Any]:
        out: Dict[str, Any] = {'kind': self.kind, 'type': self.type}
        for key, values in sorted(self.selectors.items()):
            out[key] = list(values)
        return out


@dataclass(frozen=True)
class HardwareLimitApplies:
    """Binds a hardware limit key to the board-YAML field(s) it constrains.

    Serialized inline like :class:`CapabilityMatch`: ``kind`` / ``type`` / ``path``
    / ``check`` / ``compare`` are reserved keys; every other key is a
    board-YAML selection dimension (``sub_type`` / ``role`` / ...).
    """

    _RESERVED = ('kind', 'type', 'path', 'check', 'compare')

    kind: str
    type: str
    selectors: Dict[str, List[str]] = field(default_factory=dict)
    path: List[str] = field(default_factory=list)
    check: str = 'value'
    compare: str = 'le'

    @classmethod
    def from_yaml(cls, data: Mapping[str, Any]) -> 'HardwareLimitApplies':
        selectors: Dict[str, List[str]] = {}
        for key, value in dict(data).items():
            if key in cls._RESERVED:
                continue
            selectors[str(key)] = _selector_values(value)
        return cls(
            kind=str(data.get('kind', '')).strip(),
            type=str(data.get('type', '')).strip(),
            selectors=selectors,
            path=[str(part) for part in (data.get('path', []) or [])],
            check=str(data.get('check', 'value')).strip() or 'value',
            compare=str(data.get('compare', 'le')).strip() or 'le',
        )

    def to_dict(self) -> Dict[str, Any]:
        out: Dict[str, Any] = {'kind': self.kind, 'type': self.type}
        for key, values in sorted(self.selectors.items()):
            out[key] = list(values)
        if self.path:
            out['path'] = list(self.path)
        out['check'] = self.check
        if self.compare != 'le':
            out['compare'] = self.compare
        return out


@dataclass(frozen=True)
class HardwareLimitSource:
    """One source candidate for a normalized hardware limit."""

    kind: str
    symbol: str
    value: Optional[int] = None
    path: str = ''
    component: str = ''

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> 'HardwareLimitSource':
        return cls(
            kind=str(data.get('kind', '')).strip(),
            symbol=str(data.get('symbol', '')).strip(),
            value=_optional_int(data.get('value')) if 'value' in data else None,
            path=str(data.get('path', '')).strip(),
            component=str(data.get('component', '')).strip(),
        )

    def to_dict(self) -> Dict[str, str]:
        data = {
            'kind': self.kind,
            'symbol': self.symbol,
        }
        if self.value is not None:
            data['value'] = str(self.value)
        if self.component:
            data['component'] = self.component
        if self.path:
            data['path'] = self.path
        return data


@dataclass(frozen=True)
class HardwareLimitSpec:
    """Normalized hardware limit, its extraction sources, and field bindings."""

    sources: List[HardwareLimitSource] = field(default_factory=list)
    applies_to: List[HardwareLimitApplies] = field(default_factory=list)

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> 'HardwareLimitSpec':
        return cls(
            sources=[
                HardwareLimitSource.from_dict(item)
                for item in data.get('sources', []) or []
                if isinstance(item, Mapping)
            ],
            applies_to=[
                HardwareLimitApplies.from_yaml(item)
                for item in data.get('applies_to', data.get('appliesTo', [])) or []
                if isinstance(item, Mapping)
            ],
        )

    def to_dict(self) -> Dict[str, Any]:
        data: Dict[str, Any] = {'sources': [source.to_dict() for source in self.sources]}
        if self.applies_to:
            data['appliesTo'] = [applies.to_dict() for applies in self.applies_to]
        return data


@dataclass(frozen=True)
class SocCapabilitySpec:
    """BMGR-owned SoC capability specification loaded from YAML."""

    requirement_rules: Dict[str, Dict[str, SocRequirementRule]] = field(
        default_factory=lambda: {'devices': {}, 'peripherals': {}, 'capabilities': {}}
    )
    capability_matches: Dict[str, Dict[str, CapabilityMatch]] = field(
        default_factory=lambda: {'devices': {}, 'peripherals': {}}
    )
    gpio_macros: GpioMacroSpec = field(default_factory=GpioMacroSpec)
    numeric_limits: List[str] = field(default_factory=list)
    hardware_limits: Dict[str, HardwareLimitSpec] = field(default_factory=dict)


@dataclass(frozen=True)
class GpioCapability:
    """GPIO validity derived from SoC GPIO masks."""

    valid_input: List[int] = field(default_factory=list)
    valid_output: List[int] = field(default_factory=list)
    input_range_max: Optional[int] = None
    output_range_max: Optional[int] = None
    raw_masks: Dict[str, str] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> 'GpioCapability':
        return cls(
            valid_input=[int(v) for v in data.get('validInput', [])],
            valid_output=[int(v) for v in data.get('validOutput', [])],
            input_range_max=_optional_int(data.get('inputRangeMax')),
            output_range_max=_optional_int(data.get('outputRangeMax')),
            raw_masks={str(k): str(v) for k, v in dict(data.get('rawMasks', {})).items()},
        )

    def to_dict(self) -> Dict[str, Any]:
        return {
            'validInput': list(self.valid_input),
            'validOutput': list(self.valid_output),
            'inputRangeMax': self.input_range_max,
            'outputRangeMax': self.output_range_max,
        }


@dataclass(frozen=True)
class SocChipProfile:
    """Normalized chip capability facts.

    ``idf_profile`` is the source profile for generator internals.
    """

    chip: str
    idf_profile: str
    booleans: Dict[str, bool] = field(default_factory=dict)
    numbers: Dict[str, int] = field(default_factory=dict)
    raw: Dict[str, str] = field(default_factory=dict)
    gpio: GpioCapability = field(default_factory=GpioCapability)
    capabilities: Dict[str, bool] = field(default_factory=dict)
    hardware_limits: Dict[str, int] = field(default_factory=dict)
    adc_channel_map: AdcChannelMap = field(default_factory=dict)

    @classmethod
    def from_dict(cls, chip: str, idf_profile: str, data: Mapping[str, Any]) -> 'SocChipProfile':
        return cls(
            chip=chip,
            idf_profile=idf_profile,
            booleans={str(k): bool(v) for k, v in dict(data.get('booleans', {})).items()},
            numbers={str(k): int(v) for k, v in dict(data.get('numbers', {})).items()},
            raw={str(k): str(v) for k, v in dict(data.get('sourceMacros', data.get('raw', {}))).items()},
            gpio=GpioCapability.from_dict(data.get('gpio', {})),
            capabilities={str(k): bool(v) for k, v in dict(data.get('capabilities', {})).items()},
            hardware_limits={str(k): int(v) for k, v in dict(data.get('hardwareLimits', {})).items()},
            adc_channel_map=adc_channel_map_from_catalog(data),
        )

    def supports(self, capability_key: str) -> bool:
        return bool(self.capabilities.get(capability_key, False))

    def bool(self, macro: str) -> Optional[bool]:
        return self.booleans.get(macro)

    def number(self, macro: str) -> Optional[int]:
        return self.numbers.get(macro)

    def hardware_limit(self, key: str) -> Optional[int]:
        return self.hardware_limits.get(key)

    def valid_gpio_numbers(self, direction: str = 'unknown') -> List[int]:
        if direction == 'output':
            return list(self.gpio.valid_output)
        return list(self.gpio.valid_input)

    def to_dict(self) -> Dict[str, Any]:
        data: Dict[str, Any] = {
            'capabilities': dict(sorted(self.capabilities.items())),
            'gpio': self.gpio.to_dict(),
            'hardwareLimits': dict(sorted(self.hardware_limits.items())),
        }
        if self.adc_channel_map:
            data['adcChannelMap'] = adc_channel_map_to_catalog(self.adc_channel_map)
        return data


class SocCapsParser:
    """Parse selected facts from ESP-IDF ``soc_caps.h``."""

    _define_re = re.compile(r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)(?:\([^)]*\))?\s*(.*?)\s*$')

    def __init__(self, idf_path: Path):
        self.idf_path = Path(idf_path)

    def parse_chip(
        self,
        chip: str,
        idf_profile: str = '',
        gpio_macros: Optional[GpioMacroSpec] = None,
    ) -> SocChipProfile:
        chip_norm = _normalize_chip(chip)
        caps_path = self._soc_caps_path(chip_norm)
        text = caps_path.read_text(encoding='utf-8', errors='ignore')
        raw = self._parse_defines(text)
        booleans, numbers = _split_macro_values(raw)
        gpio = _derive_gpio(raw, numbers, gpio_macros or GpioMacroSpec())
        return SocChipProfile(
            chip=chip_norm,
            idf_profile=idf_profile,
            booleans=booleans,
            numbers=numbers,
            raw=raw,
            gpio=gpio,
        )

    def _soc_caps_path(self, chip: str) -> Path:
        roots = self.idf_path / 'components' / 'soc' / chip / 'include'
        candidates = [
            roots / 'soc' / 'soc_caps.h',
            roots / 'soc_caps.h',
        ]
        for path in candidates:
            if path.is_file():
                return path
        raise FileNotFoundError(str(candidates[0]))

    def _parse_defines(self, text: str) -> Dict[str, str]:
        raw: Dict[str, str] = {}
        for original in text.splitlines():
            line = _strip_comments(original).strip()
            if not line:
                continue
            match = self._define_re.match(line)
            if not match:
                continue
            name, value = match.group(1), match.group(2).strip()
            if not name.startswith('SOC_'):
                continue
            raw[name] = value or '1'
        return raw


class SocCapabilityCatalog:
    """In-memory representation of the generated SoC capability catalog."""

    def __init__(
        self,
        profiles: Mapping[str, Mapping[str, SocChipProfile]],
        capability_rules: Optional[Mapping[str, Mapping[str, SocRequirementRule]]] = None,
        idf_profiles: Optional[Sequence[Mapping[str, Any]]] = None,
        gpio_macros: Optional[GpioMacroSpec] = None,
        numeric_limits: Optional[Sequence[str]] = None,
        hardware_limits: Optional[Mapping[str, HardwareLimitSpec]] = None,
        capability_matches: Optional[Mapping[str, Mapping[str, CapabilityMatch]]] = None,
    ):
        self._profiles: Dict[str, Dict[str, SocChipProfile]] = {
            _normalize_chip(chip): dict(profile_map) for chip, profile_map in profiles.items()
        }
        self.capability_rules: Dict[str, Dict[str, SocRequirementRule]] = {
            section: dict(rules) for section, rules in (capability_rules or {}).items()
        }
        self.capability_matches: Dict[str, Dict[str, CapabilityMatch]] = {
            section: dict(matches) for section, matches in (capability_matches or {}).items()
        }
        self.idf_profiles = list(idf_profiles or [])
        self.gpio_macros = gpio_macros or GpioMacroSpec()
        self.numeric_limits = list(numeric_limits or [])
        self.hardware_limit_specs = dict(hardware_limits or {})
        self.diagnostics: List[str] = []

    @classmethod
    def build(
        cls,
        requirement_path: Path,
        idf_profiles: Sequence[Tuple[str, Path]],
        chips: Sequence[str],
    ) -> 'SocCapabilityCatalog':
        spec = load_soc_capability_spec(requirement_path)
        rules = spec.requirement_rules
        profiles: Dict[str, Dict[str, SocChipProfile]] = {}
        profile_meta = []
        diagnostics: List[str] = []
        for profile_id, idf_path in idf_profiles:
            profile_meta.append({'id': profile_id})
            parser = SocCapsParser(Path(idf_path))
            for chip in chips:
                try:
                    parsed = parser.parse_chip(chip, profile_id, gpio_macros=spec.gpio_macros)
                except FileNotFoundError:
                    continue
                capabilities = _evaluate_capabilities(rules, parsed.booleans)
                hardware_limits = _derive_hardware_limits(
                    spec.hardware_limits,
                    parsed.numbers,
                    Path(idf_path),
                    parsed.chip,
                )
                adc_channel_map: AdcChannelMap = {}
                try:
                    adc_channel_map = parse_adc_channel_header(chip, Path(idf_path))
                except FileNotFoundError:
                    if parsed.booleans.get('SOC_ADC_SUPPORTED'):
                        diagnostics.append(
                            f'{parsed.chip}@{profile_id}: SOC_ADC_SUPPORTED but adc_channel.h is missing',
                        )
                except ValueError as exc:
                    if parsed.booleans.get('SOC_ADC_SUPPORTED'):
                        diagnostics.append(f'{parsed.chip}@{profile_id}: {exc}')
                parsed = SocChipProfile(
                    chip=parsed.chip,
                    idf_profile=profile_id,
                    booleans=parsed.booleans,
                    numbers=parsed.numbers,
                    raw=parsed.raw,
                    gpio=parsed.gpio,
                    capabilities=capabilities,
                    hardware_limits=hardware_limits,
                    adc_channel_map=adc_channel_map,
                )
                profiles.setdefault(parsed.chip, {})[profile_id] = parsed
        catalog = cls(
            profiles=profiles,
            capability_rules=rules,
            idf_profiles=profile_meta,
            gpio_macros=spec.gpio_macros,
            numeric_limits=spec.numeric_limits,
            hardware_limits=spec.hardware_limits,
            capability_matches=spec.capability_matches,
        )
        catalog.diagnostics.extend(diagnostics)
        return catalog

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> 'SocCapabilityCatalog':
        profiles: Dict[str, Dict[str, SocChipProfile]] = {}
        profile_meta = data.get('profile')
        profile_id = ''
        if isinstance(profile_meta, Mapping):
            profile_id = str(profile_meta.get('id', '')).strip()
        for chip, chip_data in dict(data.get('chips', {})).items():
            profile_map: Dict[str, SocChipProfile] = {}
            if profile_id:
                profile_map[profile_id] = SocChipProfile.from_dict(str(chip), profile_id, chip_data)
            elif 'profiles' in chip_data:
                for nested_profile_id, profile_data in dict(chip_data.get('profiles', {})).items():
                    profile_map[str(nested_profile_id)] = SocChipProfile.from_dict(
                        str(chip), str(nested_profile_id), profile_data
                    )
            else:
                supported_since = str(chip_data.get('supportedSince', ''))
                if supported_since:
                    profile_map[supported_since] = SocChipProfile.from_dict(str(chip), supported_since, chip_data)
            profiles[str(chip)] = profile_map
        rules = _rules_from_catalog_dict(data.get('capabilityRules', {}))
        if profile_id:
            idf_profiles = [{'id': profile_id}]
        else:
            idf_profiles = list(data.get('generatedFromProfiles', data.get('idfProfiles', [])))
        gpio_macros = GpioMacroSpec.from_dict(data.get('gpioMacros', {}))
        numeric_limits = [str(item) for item in data.get('numericLimits', [])]
        hardware_limits = _hardware_limit_specs_from_catalog_dict(data.get('sourceSpec', {}).get('hardwareLimits', {}))
        capability_matches = _capability_matches_from_defs(data.get('capabilityDefs', {}))
        hardware_limits = _merge_hardware_limit_defs(hardware_limits, data.get('hardwareLimitDefs', {}))
        return cls(
            profiles=profiles,
            capability_rules=rules,
            idf_profiles=idf_profiles,
            gpio_macros=gpio_macros,
            numeric_limits=numeric_limits,
            hardware_limits=hardware_limits,
            capability_matches=capability_matches,
        )

    @classmethod
    def load(cls, path: Path) -> 'SocCapabilityCatalog':
        data = json.loads(Path(path).read_text(encoding='utf-8'))
        return cls.from_dict(data)

    def profile(self, chip: str, idf_profile: str) -> SocChipProfile:
        chip_norm = _normalize_chip(chip)
        profile_map = self._profiles[chip_norm]
        if idf_profile in profile_map:
            return profile_map[idf_profile]
        raise KeyError(f'{chip_norm}: no SoC capability profile {idf_profile!r}')

    def chip(self, chip: str) -> SocChipProfile:
        chip_norm = _normalize_chip(chip)
        profile_map = self._profiles[chip_norm]
        if len(profile_map) != 1:
            raise KeyError(f'{chip_norm}: catalog has multiple profiles; select one profile first')
        return next(iter(profile_map.values()))

    def to_dict(self) -> Dict[str, Any]:
        if len(self.idf_profiles) == 1:
            profile_id = str(self.idf_profiles[0].get('id', '')).strip()
            if profile_id:
                return self.to_profile_dict(profile_id)
        return self.to_index_dict()

    def to_profile_dict(self, idf_profile: str) -> Dict[str, Any]:
        self.diagnostics = []
        profile_id = str(idf_profile)
        data: Dict[str, Any] = {
            'schemaVersion': SCHEMA_VERSION,
            'profile': {'id': profile_id},
            'capabilityDefs': self._capability_defs(),
            'hardwareLimitDefs': self._hardware_limit_defs(),
            'chips': {},
        }
        for chip, profile_map in sorted(self._profiles.items()):
            profile = profile_map.get(profile_id)
            if profile is None:
                continue
            data['chips'][chip] = profile.to_dict()
        self._emit_missing_match_diagnostics()
        return data

    def to_index_dict(self) -> Dict[str, Any]:
        profiles = []
        for meta in self.idf_profiles:
            profile_id = str(meta.get('id', '')).strip()
            if not profile_id:
                continue
            profiles.append({
                'id': profile_id,
                'version': _profile_floor_version(profile_id),
                'path': _profile_file_name(profile_id),
            })
        profiles.sort(key=lambda item: _profile_sort_key(str(item['version'])))
        return {
            'schemaVersion': INDEX_SCHEMA_VERSION,
            'catalogSchemaVersion': SCHEMA_VERSION,
            'profiles': profiles,
        }

    @staticmethod
    def select_profile_entry(index_data: Mapping[str, Any], idf_version: str) -> Mapping[str, Any]:
        active_key = _profile_sort_key(str(idf_version))
        selected: Optional[Mapping[str, Any]] = None
        for entry in sorted(
            index_data.get('profiles', []),
            key=lambda item: _profile_sort_key(str(item['version'])),
        ):
            if _profile_sort_key(str(entry['version'])) <= active_key:
                selected = entry
        if selected is None:
            raise ValueError(f'no SoC capability catalog profile for ESP-IDF {idf_version}')
        return selected

    def _capability_defs(self) -> Dict[str, Any]:
        defs: Dict[str, Any] = {}
        for section in ('devices', 'peripherals'):
            prefix = _section_prefix(section)
            for name, match in sorted(self.capability_matches.get(section, {}).items()):
                key = name if not prefix else f'{prefix}.{name}'
                defs[key] = match.to_dict()
        return defs

    def _hardware_limit_defs(self) -> Dict[str, Any]:
        return {
            key: {'appliesTo': [applies.to_dict() for applies in spec.applies_to]}
            for key, spec in sorted(self.hardware_limit_specs.items())
            if spec.applies_to
        }

    def _emit_missing_match_diagnostics(self) -> None:
        for section in ('devices', 'peripherals'):
            prefix = _section_prefix(section)
            matches = self.capability_matches.get(section, {})
            for name in sorted(self.capability_rules.get(section, {})):
                if name not in matches:
                    key = name if not prefix else f'{prefix}.{name}'
                    self.diagnostics.append(f'{key}: capability has no web match spec')


class SocCapabilityProvider:
    """Unified runtime access to SoC capability facts."""

    def __init__(self, catalog: SocCapabilityCatalog, selected_profile_id: str = ''):
        self.catalog = catalog
        self.selected_profile_id = selected_profile_id

    @classmethod
    def load_for_idf_version(
        cls,
        catalog_dir: Path,
        idf_version: str,
    ) -> 'SocCapabilityProvider':
        root = Path(catalog_dir)
        index_data = json.loads((root / 'index.json').read_text(encoding='utf-8'))
        entry = SocCapabilityCatalog.select_profile_entry(index_data, idf_version)
        catalog = SocCapabilityCatalog.load(root / str(entry['path']))
        return cls(catalog, selected_profile_id=str(entry['id']))

    def profile(self, chip: str, idf_profile: str) -> SocChipProfile:
        return self.catalog.profile(chip, idf_profile)

    def chip(self, chip: str) -> SocChipProfile:
        return self.catalog.chip(chip)


def load_soc_requirement_rules(path: Path) -> Dict[str, Dict[str, SocRequirementRule]]:
    """Load BMGR capability requirement rules from YAML."""

    return load_soc_capability_spec(path).requirement_rules


def load_soc_capability_spec(path: Path) -> SocCapabilitySpec:
    """Load the BMGR SoC capability specification from YAML."""

    data = yaml.safe_load(Path(path).read_text(encoding='utf-8')) or {}
    rules: Dict[str, Dict[str, SocRequirementRule]] = {'devices': {}, 'peripherals': {}, 'capabilities': {}}
    rules_matches: Dict[str, Dict[str, CapabilityMatch]] = {'devices': {}, 'peripherals': {}}
    for section in ('devices', 'peripherals'):
        for item in data.get(section, []) or []:
            if not isinstance(item, dict):
                continue
            for name, value in item.items():
                rules[section][str(name)] = _parse_rule_value(value)
                if isinstance(value, dict) and isinstance(value.get('match'), Mapping):
                    rules_matches[section][str(name)] = CapabilityMatch.from_yaml(
                        _match_kind(section), value['match']
                    )
    for name, value in dict(data.get('capabilities', {}) or {}).items():
        rules['capabilities'][str(name)] = _parse_rule_value(value)
    numeric_limits = _string_list(data.get('numeric_limits', data.get('numericLimits', [])))
    hardware_limits = _parse_hardware_limit_specs(
        data.get('hardware_limits', data.get('hardwareLimits', {})) or {}
    )
    gpio_data = data.get('gpio', {}) or {}
    gpio_macros = GpioMacroSpec(
        valid_input_mask=str(gpio_data.get('valid_input_mask', gpio_data.get('validInputMask', GpioMacroSpec.valid_input_mask))),
        valid_output_mask=str(gpio_data.get('valid_output_mask', gpio_data.get('validOutputMask', GpioMacroSpec.valid_output_mask))),
        input_range_max=str(gpio_data.get('input_range_max', gpio_data.get('inputRangeMax', GpioMacroSpec.input_range_max))),
        output_range_max=str(gpio_data.get('output_range_max', gpio_data.get('outputRangeMax', GpioMacroSpec.output_range_max))),
    )
    return SocCapabilitySpec(
        requirement_rules=rules,
        capability_matches=rules_matches,
        gpio_macros=gpio_macros,
        numeric_limits=numeric_limits,
        hardware_limits=hardware_limits,
    )


def _parse_rule_value(value: Any) -> SocRequirementRule:
    if isinstance(value, str):
        return SocRequirementRule(all_of=[part.strip() for part in value.split(',') if part.strip()])
    if isinstance(value, dict):
        all_of = _string_list(value.get('allOf', value.get('requires', [])))
        any_of = _string_list(value.get('anyOf', []))
        return SocRequirementRule(all_of=all_of, any_of=any_of)
    return SocRequirementRule()


def _match_kind(section: str) -> str:
    return 'device' if section == 'devices' else 'peripheral'


def _string_list(value: Any) -> List[str]:
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        return [str(item).strip() for item in value if str(item).strip()]
    return []


def _evaluate_capabilities(
    rules: Mapping[str, Mapping[str, SocRequirementRule]],
    booleans: Mapping[str, bool],
) -> Dict[str, bool]:
    capabilities: Dict[str, bool] = {}
    for section, section_rules in rules.items():
        prefix = _section_prefix(section)
        for name, rule in section_rules.items():
            key = name if not prefix else f'{prefix}.{name}'
            capabilities[key] = rule.evaluate(booleans)
    return capabilities


def _derive_hardware_limits(
    specs: Mapping[str, HardwareLimitSpec],
    numbers: Mapping[str, int],
    idf_path: Path,
    chip: str,
) -> Dict[str, int]:
    limits: Dict[str, int] = {}
    for key, spec in specs.items():
        for source in spec.sources:
            if source.kind == 'soc_caps_macro' and source.symbol in numbers:
                limits[key] = source.value if source.value is not None else numbers[source.symbol]
                break
            if source.kind == 'header_define':
                value = _extract_header_define_value(source, idf_path, chip)
                if value is not None:
                    limits[key] = source.value if source.value is not None else value
                    break
    return limits


def _extract_header_define_value(
    source: HardwareLimitSource,
    idf_path: Path,
    chip: str,
) -> Optional[int]:
    if not source.path or not source.symbol:
        return None
    header_path = idf_path / source.path.format(chip=chip)
    if not header_path.is_file():
        return None
    try:
        text = header_path.read_text(encoding='utf-8', errors='ignore')
    except OSError:
        return None
    raw = _parse_define_from_text(text, source.symbol)
    if raw is None:
        return None
    return _eval_int_expr(raw, _parse_simple_defines(text), {})


def _parse_define_from_text(text: str, symbol: str) -> Optional[str]:
    define_re = re.compile(
        r'^\s*#\s*define\s+'
        + re.escape(symbol)
        + r'(?:\([^)]*\))?\s+(.*?)\s*$',
        re.M,
    )
    match = define_re.search(text)
    if not match:
        return None
    return _strip_comments(match.group(1)).strip()


def _parse_simple_defines(text: str) -> Dict[str, str]:
    raw: Dict[str, str] = {}
    define_re = re.compile(r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)(?:\([^)]*\))?\s*(.*?)\s*$')
    for original in text.splitlines():
        line = _strip_comments(original).strip()
        if not line:
            continue
        match = define_re.match(line)
        if not match:
            continue
        name, value = match.group(1), match.group(2).strip()
        raw[name] = value or '1'
    return raw


def _parse_hardware_limit_specs(value: Mapping[str, Any]) -> Dict[str, HardwareLimitSpec]:
    specs: Dict[str, HardwareLimitSpec] = {}
    for key, item in dict(value).items():
        if isinstance(item, Mapping):
            spec = HardwareLimitSpec.from_dict(item)
            if spec.sources:
                specs[str(key)] = spec
    return specs


def _hardware_limit_specs_from_catalog_dict(value: Mapping[str, Any]) -> Dict[str, HardwareLimitSpec]:
    return _parse_hardware_limit_specs(value)


def _capability_matches_from_defs(defs: Mapping[str, Any]) -> Dict[str, Dict[str, CapabilityMatch]]:
    matches: Dict[str, Dict[str, CapabilityMatch]] = {'devices': {}, 'peripherals': {}}
    for key, value in dict(defs).items():
        if not isinstance(value, Mapping):
            continue
        prefix, _, name = str(key).partition('.')
        section = 'devices' if prefix == 'device' else 'peripherals' if prefix == 'peripheral' else None
        if section is None or not name:
            continue
        matches[section][name] = CapabilityMatch.from_yaml(prefix, value)
    return matches


def _merge_hardware_limit_defs(
    hardware_limits: Dict[str, HardwareLimitSpec],
    defs: Mapping[str, Any],
) -> Dict[str, HardwareLimitSpec]:
    merged = dict(hardware_limits)
    for key, value in dict(defs).items():
        if not isinstance(value, Mapping):
            continue
        applies = [
            HardwareLimitApplies.from_yaml(item)
            for item in value.get('appliesTo', value.get('applies_to', [])) or []
            if isinstance(item, Mapping)
        ]
        existing = merged.get(str(key))
        sources = existing.sources if existing else []
        merged[str(key)] = HardwareLimitSpec(sources=list(sources), applies_to=applies)
    return merged


def _section_prefix(section: str) -> str:
    if section == 'devices':
        return 'device'
    if section == 'peripherals':
        return 'peripheral'
    if section == 'capabilities':
        return ''
    return section.rstrip('s')


def _rules_from_catalog_dict(data: Mapping[str, Any]) -> Dict[str, Dict[str, SocRequirementRule]]:
    rules: Dict[str, Dict[str, SocRequirementRule]] = {'devices': {}, 'peripherals': {}}
    for key, value in dict(data).items():
        prefix, _, name = str(key).partition('.')
        section = 'devices' if prefix == 'device' else 'peripherals' if prefix == 'peripheral' else 'capabilities'
        rules.setdefault(section, {})[name if section != 'capabilities' else str(key)] = _parse_rule_value(value)
    return rules


def _split_macro_values(raw: Mapping[str, str]) -> Tuple[Dict[str, bool], Dict[str, int]]:
    booleans: Dict[str, bool] = {}
    numbers: Dict[str, int] = {}
    for name, value in raw.items():
        number = _eval_int_expr(value, raw, {})
        if number is not None:
            numbers[name] = number
            booleans[name] = number != 0
        else:
            booleans[name] = True
    return booleans, numbers


def _derive_gpio(
    raw: Mapping[str, str],
    numbers: Mapping[str, int],
    gpio_macros: GpioMacroSpec,
) -> GpioCapability:
    eval_cache: Dict[str, int] = dict(numbers)
    input_mask = _eval_int_expr(raw.get(gpio_macros.valid_input_mask, ''), raw, eval_cache)
    output_mask = _eval_int_expr(raw.get(gpio_macros.valid_output_mask, ''), raw, eval_cache)
    input_range_max = numbers.get(gpio_macros.input_range_max)
    output_range_max = numbers.get(gpio_macros.output_range_max)
    return GpioCapability(
        valid_input=_mask_to_numbers(input_mask),
        valid_output=_mask_to_numbers(output_mask),
        input_range_max=input_range_max,
        output_range_max=output_range_max,
        raw_masks={
            key: raw[key]
            for key in (gpio_macros.valid_input_mask, gpio_macros.valid_output_mask)
            if key in raw
        },
    )


def _mask_to_numbers(mask: Optional[int]) -> List[int]:
    if mask is None or mask < 0:
        return []
    return [idx for idx in range(mask.bit_length()) if mask & (1 << idx)]


def _eval_int_expr(expr: str, raw: Mapping[str, str], cache: Mapping[str, int]) -> Optional[int]:
    if not expr:
        return None
    normalized = _normalize_c_int_expr(expr)
    try:
        tree = ast.parse(normalized, mode='eval')
    except SyntaxError:
        return None
    evaluator = _SafeIntEvaluator(raw, dict(cache))
    try:
        return evaluator.visit(tree)
    except (ValueError, RecursionError):
        return None


class _SafeIntEvaluator(ast.NodeVisitor):
    def __init__(self, raw: Mapping[str, str], cache: Dict[str, int]):
        self.raw = raw
        self.cache = cache

    def visit_Expression(self, node: ast.Expression) -> int:
        return self.visit(node.body)

    def visit_Constant(self, node: ast.Constant) -> int:
        if isinstance(node.value, int):
            return int(node.value)
        raise ValueError('unsupported constant')

    def visit_UnaryOp(self, node: ast.UnaryOp) -> int:
        operand = self.visit(node.operand)
        if isinstance(node.op, ast.Invert):
            return ~operand
        if isinstance(node.op, ast.USub):
            return -operand
        if isinstance(node.op, ast.UAdd):
            return operand
        raise ValueError('unsupported unary op')

    def visit_BinOp(self, node: ast.BinOp) -> int:
        left = self.visit(node.left)
        right = self.visit(node.right)
        if isinstance(node.op, ast.BitAnd):
            return left & right
        if isinstance(node.op, ast.BitOr):
            return left | right
        if isinstance(node.op, ast.LShift):
            return left << right
        if isinstance(node.op, ast.RShift):
            return left >> right
        if isinstance(node.op, ast.Add):
            return left + right
        if isinstance(node.op, ast.Sub):
            return left - right
        raise ValueError('unsupported binary op')

    def visit_Name(self, node: ast.Name) -> int:
        name = node.id
        if name in self.cache:
            return self.cache[name]
        if name.startswith('BIT') and name[3:].isdigit():
            value = 1 << int(name[3:])
            self.cache[name] = value
            return value
        if name not in self.raw:
            raise ValueError(f'unknown macro {name}')
        value = _eval_int_expr(self.raw[name], self.raw, self.cache)
        if value is None:
            raise ValueError(f'unresolved macro {name}')
        self.cache[name] = value
        return value

    def generic_visit(self, node: ast.AST) -> int:
        raise ValueError(f'unsupported expression: {type(node).__name__}')


def _normalize_c_int_expr(expr: str) -> str:
    text = _strip_comments(expr).strip()
    text = re.sub(r'\b(0[xX][0-9A-Fa-f]+|\d+)(?:ULL|LLU|UL|LU|U|L)\b', r'\1', text)
    return text


def _strip_comments(line: str) -> str:
    line = line.split('//', 1)[0]
    line = re.sub(r'/\*.*?\*/', '', line)
    return line


def _normalize_chip(chip: str) -> str:
    return chip.strip().lower().replace('-', '')


def _first_profile_id(profile_map: Mapping[str, SocChipProfile]) -> str:
    if not profile_map:
        raise KeyError('chip has no IDF profiles')
    return sorted(profile_map.keys(), key=_profile_sort_key)[0]


def _profile_sort_key(profile_id: str) -> Tuple[int, ...]:
    parts = re.findall(r'\d+', str(profile_id))
    if not parts:
        return (999999,)
    numbers = [int(part) for part in parts]
    # Pad to 3 parts so short versions ('5.5') compare correctly against the
    # 3-part versions produced by _profile_floor_version ('5.5.0'). Without
    # padding, (5, 5) < (5, 5, 0) would skip the matching profile.
    while len(numbers) < 3:
        numbers.append(0)
    return tuple(numbers)


def _profile_file_name(profile_id: str) -> str:
    safe = str(profile_id).strip().lower().replace('.', '_').replace('-', '_')
    safe = safe.replace('+', '_').replace('/', '_')
    safe = re.sub(r'[^a-z0-9_]+', '_', safe).strip('_')
    return f'idf_{safe}.json'


def _profile_floor_version(profile_id: str) -> str:
    text = str(profile_id).strip().lower()
    if text.endswith('.x'):
        text = text[:-2]
    parts = [part for part in re.split(r'[^0-9]+', text) if part]
    numbers = [int(part) for part in parts[:3]]
    while len(numbers) < 3:
        numbers.append(0)
    return '.'.join(str(part) for part in numbers)


def _optional_int(value: Any) -> Optional[int]:
    if value is None:
        return None
    return int(value)
