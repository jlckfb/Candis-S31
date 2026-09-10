# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from generators.utils import soc_capability_query as query


def _write_catalog(root: Path) -> Path:
    catalog_dir = root / 'soc_capability_catalog'
    catalog_dir.mkdir()
    (catalog_dir / 'index.json').write_text(
        json.dumps({
            'schemaVersion': 1,
            'catalogSchemaVersion': 3,
            'profiles': [
                {'id': '5.4', 'version': '5.4.0', 'path': 'idf_5_4.json'},
                {'id': '5.5', 'version': '5.5.0', 'path': 'idf_5_5.json'},
            ],
        }),
        encoding='utf-8',
    )
    common = {
        'schemaVersion': 3,
        'capabilityDefs': {
            'peripheral.i2c': {'kind': 'peripheral', 'type': 'i2c'},
            'peripheral.i2s_master_pdm-in': {
                'kind': 'peripheral',
                'type': 'i2s',
                'format': ['pdm-in'],
            },
            'device.display_lcd_rgb': {
                'kind': 'device',
                'type': 'display_lcd',
                'sub_type': ['rgb'],
            },
        },
        'hardwareLimitDefs': {},
    }
    (catalog_dir / 'idf_5_4.json').write_text(
        json.dumps({
            **common,
            'profile': {'id': '5.4'},
            'chips': {
                'esp32s3': {
                    'capabilities': {
                        'peripheral.i2c': False,
                        'peripheral.i2s_master_pdm-in': False,
                        'device.display_lcd_rgb': False,
                    },
                    'gpio': {'validInput': [0, 1], 'validOutput': [0]},
                    'hardwareLimits': {'i2c.instance_count': 1},
                },
            },
        }),
        encoding='utf-8',
    )
    (catalog_dir / 'idf_5_5.json').write_text(
        json.dumps({
            **common,
            'profile': {'id': '5.5'},
            'chips': {
                'esp32s3': {
                    'capabilities': {
                        'peripheral.i2c': True,
                        'peripheral.i2s_master_pdm-in': True,
                        'device.display_lcd_rgb': True,
                    },
                    'gpio': {'validInput': [0, 1, 2], 'validOutput': [0, 1]},
                    'hardwareLimits': {'i2c.instance_count': 2},
                },
            },
        }),
        encoding='utf-8',
    )
    return catalog_dir


def test_configure_selects_profile_and_exposes_chip_facts(tmp_path: Path) -> None:
    catalog_dir = _write_catalog(tmp_path)

    query.configure_soc_capabilities(catalog_dir=catalog_dir, idf_version='5.5.2', chip='ESP32-S3')

    soc = query.current_soc()

    assert soc.supports('peripheral.i2c') is True
    assert soc.supports('i2c.lp_supported', default=True) is True
    assert soc.supports('i2c.lp_supported', default=False) is False
    assert soc.limit('i2c.instance_count') == 2
    assert soc.valid_gpio([0, 1], direction='output') is True
    assert soc.valid_gpio([0, 2], direction='output') is False
    assert soc.valid_gpio('GPIO_NUM_1', direction='output') is True
    assert soc.valid_gpio('GPIO_NUM_2', direction='output') is False
    assert soc.valid_gpio('GPIO_NUM_NC', direction='output') is True
    assert soc.valid_gpio(None) is True
    assert query.soc_supports('peripheral.i2c') is True
    assert query.soc_hardware_limit('i2c.instance_count') == 2
    assert query.soc_valid_gpio(2, direction='input') is True
    assert query.soc_valid_gpio(2, direction='output') is False
    assert query.current_soc_catalog() is not None


def test_unconfigured_query_returns_none_not_false() -> None:
    query.clear_soc_capabilities()

    soc = query.current_soc()

    assert soc.supports('peripheral.i2c') is False
    assert soc.limit('i2c.instance_count') is None
    assert soc.valid_gpio([0, 1], direction='output') is True
    assert query.soc_supports('peripheral.i2c') is None
    assert query.soc_hardware_limit('i2c.instance_count') is None
    assert query.soc_valid_gpio(0, direction='input') is None
    assert query.current_soc_catalog() is None


def test_missing_catalog_returns_none() -> None:
    query.clear_soc_capabilities()
    query.configure_soc_capabilities(
        catalog_dir=Path('/does/not/exist'),
        idf_version='5.5',
        chip='esp32s3',
    )

    assert query.soc_supports('peripheral.i2c') is None
    assert query.current_soc_catalog() is None


def test_validate_soc_availability_uses_capability_defs(tmp_path: Path) -> None:
    catalog_dir = _write_catalog(tmp_path)
    query.configure_soc_capabilities(catalog_dir=catalog_dir, idf_version='5.5.2', chip='esp32s3')

    query.validate_soc_availability(
        'peripheral',
        {'type': 'i2s', 'format': 'pdm-in'},
        label='i2s_mic',
    )
    query.validate_soc_availability(
        'peripheral',
        {'type': 'i2s', 'format': ['std-out', 'pdm-in']},
        label='i2s_mic',
    )
    query.validate_soc_availability(
        'device',
        {'type': 'display_lcd', 'sub_type': 'rgb'},
        label='lcd',
    )


def test_validate_soc_availability_rejects_explicit_false(tmp_path: Path) -> None:
    catalog_dir = _write_catalog(tmp_path)
    query.configure_soc_capabilities(catalog_dir=catalog_dir, idf_version='5.4.4', chip='esp32s3')

    try:
        query.validate_soc_availability(
            'peripheral',
            {'type': 'i2s', 'format': 'pdm-in'},
            label='i2s_mic',
        )
    except ValueError as exc:
        message = str(exc)
    else:
        raise AssertionError('expected unsupported SoC capability to raise ValueError')

    assert "Peripheral 'i2s_mic'" in message
    assert 'type=i2s' in message
    assert 'format=pdm-in' in message
    assert 'peripheral.i2s_master_pdm-in' in message
    assert 'esp32s3' in message
    assert '5.4' in message


def test_validate_soc_availability_rejects_any_unsupported_match(tmp_path: Path) -> None:
    catalog_dir = _write_catalog(tmp_path)
    catalog_path = catalog_dir / 'idf_5_5.json'
    data = json.loads(catalog_path.read_text(encoding='utf-8'))
    data['capabilityDefs'] = {
        'peripheral.i2s_master_std': {
            'kind': 'peripheral',
            'type': 'i2s',
            'format': ['std-out'],
        },
        'peripheral.i2s_master_pdm-in': {
            'kind': 'peripheral',
            'type': 'i2s',
            'format': ['pdm-in'],
        },
    }
    data['chips']['esp32s3']['capabilities']['peripheral.i2s_master_std'] = True
    data['chips']['esp32s3']['capabilities']['peripheral.i2s_master_pdm-in'] = False
    catalog_path.write_text(json.dumps(data), encoding='utf-8')

    query.configure_soc_capabilities(catalog_dir=catalog_dir, idf_version='5.5.2', chip='esp32s3')

    with pytest.raises(ValueError, match='peripheral.i2s_master_pdm-in'):
        query.validate_soc_availability(
            'peripheral',
            {'type': 'i2s', 'format': ['std-out', 'pdm-in']},
            label='i2s_mixed',
        )


def test_validate_soc_availability_fails_open_for_missing_context_or_match(tmp_path: Path) -> None:
    query.clear_soc_capabilities()
    query.validate_soc_availability(
        'peripheral',
        {'type': 'i2s', 'format': 'pdm-in'},
        label='i2s_mic',
    )

    catalog_dir = _write_catalog(tmp_path)
    query.configure_soc_capabilities(catalog_dir=catalog_dir, idf_version='5.4.4', chip='esp32s3')
    query.validate_soc_availability(
        'peripheral',
        {'type': 'unknown_type'},
        label='legacy',
    )
    query.validate_soc_availability(
        'peripheral',
        {'type': 'i2s', 'format': 'std-out'},
        label='i2s_std',
    )
