# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from generators import adc_channel_mapper
from generators.utils import soc_capability_query as query
from generators.utils.soc_capabilities import SocCapabilityCatalog


def _configure_catalog(tmp_path: Path, *, adc_channel_map: dict | None) -> None:
    catalog_dir = tmp_path / 'soc_capability_catalog'
    catalog_dir.mkdir()
    chip_data = {
        'capabilities': {},
        'gpio': {'validInput': [0, 1, 2], 'validOutput': [0, 1, 2]},
        'hardwareLimits': {},
    }
    if adc_channel_map is not None:
        chip_data['adcChannelMap'] = adc_channel_map

    (catalog_dir / 'index.json').write_text(
        json.dumps({
            'schemaVersion': 1,
            'catalogSchemaVersion': 3,
            'profiles': [{'id': '5.5', 'version': '5.5.0', 'path': 'idf_5_5.json'}],
        }),
        encoding='utf-8',
    )
    catalog = SocCapabilityCatalog.from_dict({
        'schemaVersion': 3,
        'profile': {'id': '5.5'},
        'capabilityDefs': {},
        'hardwareLimitDefs': {},
        'chips': {'esp32s3': chip_data},
    })
    (catalog_dir / 'idf_5_5.json').write_text(json.dumps(catalog.to_dict()), encoding='utf-8')
    query.configure_soc_capabilities(catalog_dir=catalog_dir, idf_version='5.5.1', chip='esp32s3')


def test_adc_channel_mapper_reads_catalog_map(tmp_path: Path) -> None:
    _configure_catalog(
        tmp_path,
        adc_channel_map={'1': {'0': 36, '4': 32}},
    )

    assert adc_channel_mapper.adc_channel_to_gpio('esp32s3', 'ADC_UNIT_1', 4) == 32
    assert adc_channel_mapper.adc_channels_to_gpios('esp32s3', 'ADC_UNIT_1', [0, 4]) == [36, 32]


def test_adc_channel_mapper_requires_configured_catalog(tmp_path: Path) -> None:
    query.clear_soc_capabilities()

    with pytest.raises(FileNotFoundError, match='catalog is not configured'):
        adc_channel_mapper.adc_channel_to_gpio('esp32s3', 'ADC_UNIT_1', 0)


def test_adc_channel_mapper_requires_chip_map(tmp_path: Path) -> None:
    _configure_catalog(tmp_path, adc_channel_map=None)

    with pytest.raises(FileNotFoundError, match='ADC channel map is not available'):
        adc_channel_mapper.adc_channel_to_gpio('esp32s3', 'ADC_UNIT_1', 0)


def test_adc_channel_mapper_rejects_zero_unit_string() -> None:
    with pytest.raises(ValueError, match='Unsupported ADC unit value: ADC_UNIT_0'):
        adc_channel_mapper._normalize_unit('ADC_UNIT_0')
