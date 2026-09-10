# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from generators.utils.adc_channel_map_parser import (
    adc_channel_map_from_catalog,
    adc_channel_map_to_catalog,
    parse_adc_channel_header,
    parse_adc_channel_header_text,
)


def test_parse_adc_channel_header_text_extracts_unit_channel_gpio() -> None:
    text = """
    #define ADC1_CHANNEL_0_GPIO_NUM 36
    #define ADC1_CHANNEL_4_GPIO_NUM 32
    #define ADC2_CHANNEL_0_GPIO_NUM 4
    #define ADC1_CHANNEL_9_GPIO_NUM -1
    """
    mapping = parse_adc_channel_header_text(text)

    assert mapping == {
        1: {0: 36, 4: 32},
        2: {0: 4},
    }
    assert adc_channel_map_to_catalog(mapping) == {
        '1': {'0': 36, '4': 32},
        '2': {'0': 4},
    }


def test_adc_channel_map_from_catalog_round_trip() -> None:
    catalog = {
        'adcChannelMap': {
            '1': {'0': 36, '4': 32},
            '2': {'0': 4},
        },
    }
    assert adc_channel_map_from_catalog(catalog) == {
        1: {0: 36, 4: 32},
        2: {0: 4},
    }


def test_parse_adc_channel_header_reads_idf_layout(tmp_path: Path) -> None:
    idf = tmp_path / 'idf'
    header = idf / 'components' / 'soc' / 'esp32s3' / 'include' / 'soc' / 'adc_channel.h'
    header.parent.mkdir(parents=True)
    header.write_text(
        '#define ADC1_CHANNEL_0_GPIO_NUM 1\n#define ADC1_CHANNEL_1_GPIO_NUM 2\n',
        encoding='utf-8',
    )

    assert parse_adc_channel_header('esp32s3', idf) == {1: {0: 1, 1: 2}}


def test_parse_adc_channel_header_missing_file_raises(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError, match='adc_channel.h'):
        parse_adc_channel_header('esp32s3', tmp_path / 'idf')


def test_parse_adc_channel_header_empty_macros_raise(tmp_path: Path) -> None:
    idf = tmp_path / 'idf'
    header = idf / 'components' / 'soc' / 'esp32s3' / 'include' / 'soc' / 'adc_channel.h'
    header.parent.mkdir(parents=True)
    header.write_text('/* no macros */\n', encoding='utf-8')

    with pytest.raises(ValueError, match='No ADC channel mappings'):
        parse_adc_channel_header('esp32s3', idf)
