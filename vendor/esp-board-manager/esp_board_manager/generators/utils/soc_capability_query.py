# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

"""Shared static SoC capability query facade for BMGR parsers and validators."""

from __future__ import annotations

from collections.abc import Iterable
from pathlib import Path
from typing import Any, Mapping, Optional, Union

from .soc_capabilities import SocCapabilityCatalog, SocCapabilityProvider, SocChipProfile


_provider: Optional[SocCapabilityProvider] = None
_chip: str = ''


class SocQueryContext:
    """Parser-facing SoC capability query API.

    Missing catalog data is treated as unknown. Boolean checks default to false,
    numeric limits return the caller-provided default, and GPIO checks default to
    true so older/unconfigured environments do not reject existing boards.
    """

    def __init__(self, chip_profile: Optional[SocChipProfile] = None):
        self._chip_profile = chip_profile

    def supports(self, capability_key: str, default: bool = False) -> bool:
        if self._chip_profile is None:
            return bool(default)
        key = str(capability_key)
        if key not in self._chip_profile.capabilities:
            return bool(default)
        return bool(self._chip_profile.supports(key))

    def limit(self, limit_key: str, default: Optional[int] = None) -> Optional[int]:
        if self._chip_profile is None:
            return default
        value = self._chip_profile.hardware_limit(str(limit_key))
        return default if value is None else value

    def valid_gpio(
        self,
        pins: Union[int, Iterable],
        direction: str = 'any',
        allow_nc: bool = True,
        default: bool = True,
    ) -> bool:
        if self._chip_profile is None:
            return bool(default)

        for pin in _pin_iter(pins):
            if _is_nc_pin(pin):
                if allow_nc:
                    continue
                return False
            pin_num = _pin_number(pin)
            if pin_num is None:
                return False
            valid = _valid_gpio_pin(self._chip_profile, pin_num, direction)
            if not valid:
                return False
        return True


def _normalize_chip(chip: Optional[str]) -> str:
    return str(chip or '').strip().lower().replace('-', '')


def clear_soc_capabilities() -> None:
    global _provider, _chip
    _provider = None
    _chip = ''


def configure_soc_capabilities(catalog_dir: Path, idf_version: str, chip: str) -> None:
    global _provider, _chip
    clear_soc_capabilities()
    try:
        _provider = SocCapabilityProvider.load_for_idf_version(Path(catalog_dir), str(idf_version))
        _chip = _normalize_chip(chip)
    except (OSError, ValueError, KeyError, TypeError):
        _provider = None
        _chip = ''


def current_soc_catalog() -> Optional[SocCapabilityCatalog]:
    if _provider is None:
        return None
    return _provider.catalog


def current_soc_chip_name() -> str:
    return _chip


def current_soc_chip() -> Optional[SocChipProfile]:
    if _provider is None or not _chip:
        return None
    try:
        return _provider.chip(_chip)
    except KeyError:
        return None


def current_soc() -> SocQueryContext:
    return SocQueryContext(current_soc_chip())


def validate_soc_availability(kind: str, item, *, label: str = '') -> None:
    """Validate device/peripheral availability against catalog ``capabilityDefs``.

    This is an entrance-level check. It intentionally fails open when the catalog,
    selected chip profile, or matching definition is unavailable so older or
    partially configured flows keep their parser behavior.
    """
    catalog = current_soc_catalog()
    chip = current_soc_chip()
    if catalog is None or chip is None:
        return

    normalized_kind = _normalize_kind(kind)
    item_type = _item_value(item, 'type')
    if not normalized_kind or not item_type:
        return

    section = 'devices' if normalized_kind == 'device' else 'peripherals'
    matches = getattr(catalog, 'capability_matches', {}).get(section, {})
    matched = False
    for name, match in matches.items():
        if match.type != str(item_type):
            continue
        if not _selectors_match(item, match.selectors):
            continue
        matched = True
        capability_key = '%s.%s' % (normalized_kind, name)
        if not current_soc().supports(capability_key, default=True):
            raise ValueError(_availability_error(normalized_kind, label, item, capability_key, chip))
    if matched:
        return


def soc_supports(capability_key: str) -> Optional[bool]:
    chip = current_soc_chip()
    if chip is None:
        return None
    return chip.supports(str(capability_key))


def soc_hardware_limit(limit_key: str) -> Optional[int]:
    chip = current_soc_chip()
    if chip is None:
        return None
    return chip.hardware_limit(str(limit_key))


def soc_valid_gpio(pin: int, direction: str = 'input') -> Optional[bool]:
    chip = current_soc_chip()
    if chip is None:
        return None
    pin_num = int(pin)
    return _valid_gpio_pin(chip, pin_num, direction)


def _valid_gpio_pin(chip: SocChipProfile, pin_num: int, direction: str) -> bool:
    if direction == 'output':
        return pin_num in chip.gpio.valid_output
    if direction == 'input':
        return pin_num in chip.gpio.valid_input
    return pin_num in set(chip.gpio.valid_input) | set(chip.gpio.valid_output)


def _normalize_kind(kind: str) -> str:
    value = str(kind or '').strip().lower()
    if value in ('device', 'devices'):
        return 'device'
    if value in ('peripheral', 'peripherals'):
        return 'peripheral'
    return ''


def _item_value(item, key: str):
    if isinstance(item, Mapping):
        return item.get(key)
    return getattr(item, key, None)


def _value_list(value) -> list:
    if value is None:
        return []
    if isinstance(value, (str, bytes)):
        return [str(value)]
    try:
        return [str(item) for item in value]
    except TypeError:
        return [str(value)]


def _selectors_match(item, selectors: Mapping[str, Any]) -> bool:
    for key, expected_values in selectors.items():
        actual_values = _value_list(_item_value(item, key))
        if not actual_values:
            return False
        expected = set(str(value) for value in expected_values)
        if not any(value in expected for value in actual_values):
            return False
    return True


def _availability_error(kind: str, label: str, item, capability_key: str, chip: SocChipProfile) -> str:
    display_kind = 'Device' if kind == 'device' else 'Peripheral'
    display_label = label or str(_item_value(item, 'name') or _item_value(item, 'type') or '<unnamed>')
    selectors = _selector_summary(item)
    return (
        "%s '%s' (%s) is not supported by chip %s in SoC capability profile %s: %s"
        % (display_kind, display_label, selectors, chip.chip, chip.idf_profile, capability_key)
    )


def _selector_summary(item) -> str:
    parts = []
    for key in ('type', 'sub_type', 'role', 'format'):
        value = _item_value(item, key)
        if value is None or value == '':
            continue
        values = _value_list(value)
        parts.append('%s=%s' % (key, ','.join(values)))
    return ', '.join(parts) if parts else 'type=<unknown>'


def _pin_iter(pins: Union[int, Iterable]) -> Iterable:
    if pins is None:
        return (pins,)
    if isinstance(pins, (str, bytes)):
        return (pins,)
    try:
        int(pins)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return pins
    return (pins,)


def _is_nc_pin(pin) -> bool:
    if pin is None:
        return True
    if isinstance(pin, int):
        return pin < 0
    if isinstance(pin, str):
        return pin.strip() in ('', '-1', 'GPIO_NUM_NC', 'IO_EXPANDER_PIN_NUM_NC')
    return False


def _pin_number(pin) -> Optional[int]:
    if isinstance(pin, str):
        value = pin.strip()
        prefix = 'GPIO_NUM_'
        if value.startswith(prefix):
            value = value[len(prefix):]
        try:
            return int(value)
        except ValueError:
            return None
    try:
        return int(pin)
    except (TypeError, ValueError):
        return None
