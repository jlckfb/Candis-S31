# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

"""Parse ESP-IDF ``adc_channel.h`` into unit/channel GPIO maps for catalog export."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any, Dict, Mapping

ADC_CHANNEL_MACRO_RE = re.compile(
    r'#define\s+ADC(?P<unit>\d)_CHANNEL_(?P<channel>\d+)_GPIO_NUM\s+(?P<gpio>-?\d+)'
)

AdcChannelMap = Dict[int, Dict[int, int]]


def adc_channel_header_path(idf_path: Path, chip: str) -> Path:
    chip_norm = str(chip).strip().lower().replace('-', '')
    return idf_path / 'components' / 'soc' / chip_norm / 'include' / 'soc' / 'adc_channel.h'


def parse_adc_channel_header_text(text: str) -> AdcChannelMap:
    mapping: AdcChannelMap = {}
    for line in text.splitlines():
        match = ADC_CHANNEL_MACRO_RE.search(line)
        if not match:
            continue
        unit = int(match.group('unit'))
        channel = int(match.group('channel'))
        gpio = int(match.group('gpio'))
        if gpio < 0:
            continue
        mapping.setdefault(unit, {})[channel] = gpio
    return mapping


def parse_adc_channel_header(chip: str, idf_path: Path) -> AdcChannelMap:
    header_path = adc_channel_header_path(idf_path, chip)
    if not header_path.is_file():
        raise FileNotFoundError(f'ADC channel header not found: {header_path}')
    mapping = parse_adc_channel_header_text(header_path.read_text(encoding='utf-8'))
    if not mapping:
        raise ValueError(f'No ADC channel mappings found in {header_path}')
    return mapping


def adc_channel_map_to_catalog(mapping: AdcChannelMap) -> Dict[str, Dict[str, int]]:
    return {
        str(unit): {str(channel): gpio for channel, gpio in sorted(channels.items())}
        for unit, channels in sorted(mapping.items())
    }


def adc_channel_map_from_catalog(data: Mapping[str, Any]) -> AdcChannelMap:
    raw = data.get('adcChannelMap')
    if not isinstance(raw, Mapping):
        return {}
    mapping: AdcChannelMap = {}
    for unit_key, channels in raw.items():
        if not isinstance(channels, Mapping):
            continue
        unit = int(str(unit_key))
        unit_map: Dict[int, int] = {}
        for channel_key, gpio in channels.items():
            gpio_int = int(gpio)
            if gpio_int < 0:
                continue
            unit_map[int(str(channel_key))] = gpio_int
        if unit_map:
            mapping[unit] = unit_map
    return mapping
