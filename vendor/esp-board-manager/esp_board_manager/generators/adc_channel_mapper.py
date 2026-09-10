#!/usr/bin/env python3
"""
# SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.
"""

"""ADC channel-to-GPIO mapping helpers backed by the bundled SoC capability catalog."""

import re
from typing import Iterable, List

from generators.utils.soc_capability_query import current_soc_chip

ADC_UNIT_RE = re.compile(r'ADC_UNIT_(?P<unit>\d+)')


def _normalize_chip_name(chip_name: str) -> str:
    if not chip_name:
        raise ValueError('chip name is required for ADC metadata extraction')
    return str(chip_name).strip().lower().replace('-', '')


def _normalize_unit(unit: object) -> int:
    if isinstance(unit, int):
        if unit <= 0:
            raise ValueError(f'Unsupported ADC unit value: {unit}')
        return unit

    if isinstance(unit, str):
        match = ADC_UNIT_RE.fullmatch(unit.strip())
        if match:
            parsed = int(match.group('unit'))
            if parsed > 0:
                return parsed
        if unit.strip().isdigit():
            parsed = int(unit.strip())
            if parsed > 0:
                return parsed

    raise ValueError(f'Unsupported ADC unit value: {unit}')


def _load_adc_channel_map(chip_name: str) -> dict:
    _normalize_chip_name(chip_name)
    profile = current_soc_chip()
    if profile is None:
        raise FileNotFoundError(
            'SoC capability catalog is not configured; configure_soc_capabilities() '
            'must run before ADC channel mapping.',
        )
    if not profile.adc_channel_map:
        chip = profile.chip or chip_name
        raise FileNotFoundError(
            f'ADC channel map is not available for chip {chip} in the selected '
            'SoC capability catalog profile.',
        )
    return profile.adc_channel_map


def adc_channel_to_gpio(chip_name: str, unit: object, channel: int) -> int:
    channel_map = _load_adc_channel_map(chip_name)
    unit_num = _normalize_unit(unit)
    gpio = channel_map.get(unit_num, {}).get(int(channel))
    if gpio is None or gpio < 0:
        raise ValueError(
            f'Unable to map ADC unit {unit} channel {channel} to GPIO for chip {chip_name}',
        )
    return gpio


def _dedupe_preserve(values: Iterable[int]) -> List[int]:
    ordered = []
    seen = set()
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        ordered.append(value)
    return ordered


def adc_channels_to_gpios(chip_name: str, unit: object, channels: Iterable[int]) -> List[int]:
    gpios = []
    for channel in channels:
        gpios.append(adc_channel_to_gpio(chip_name, unit, int(channel)))
    return _dedupe_preserve(gpios)


def adc_patterns_to_gpios(chip_name: str, patterns: Iterable[dict]) -> List[int]:
    gpios = []
    for item in patterns:
        gpios.append(
            adc_channel_to_gpio(
                chip_name,
                item.get('unit', 'ADC_UNIT_1'),
                int(item.get('channel', -1)),
            ),
        )
    return _dedupe_preserve(gpios)
