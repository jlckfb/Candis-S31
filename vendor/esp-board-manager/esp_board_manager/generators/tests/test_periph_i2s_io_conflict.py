# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

import sys
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from peripherals.periph_i2s.periph_i2s import is_duplicate_io_conflict


def _i2s_record(
    name: str,
    io_field: str,
    *,
    port: int = 0,
    format_name: str = 'std-out',
    direction: Optional[str] = None,
):
    struct_init = {
        'port': f'I2S_NUM_{port}',
    }
    if direction is not None:
        struct_init['direction'] = direction

    return {
        'owner_name': name,
        'owner_type': 'i2s',
        'io_field': io_field,
        'artifact': {
            'name': name,
            'type': 'i2s',
            'format': format_name,
            'raw': {'config': {'port': port}},
            'result': {'struct_init': struct_init},
        },
    }


def test_same_field_std_tx_rx_same_port_is_not_conflict() -> None:
    pair = [
        _i2s_record('i2s_out', 'mclk', format_name='std-out'),
        _i2s_record('i2s_in', 'mclk', format_name='std-in'),
    ]
    assert is_duplicate_io_conflict(pair) is False


def test_same_field_tdm_tx_rx_same_port_is_not_conflict() -> None:
    pair = [
        _i2s_record('tdm_out', 'bclk', format_name='tdm-out'),
        _i2s_record('tdm_in', 'bclk', format_name='tdm-in'),
    ]
    assert is_duplicate_io_conflict(pair) is False


def test_same_field_pdm_tx_rx_same_port_clk_is_not_conflict() -> None:
    pair = [
        _i2s_record('pdm_out', 'clk', format_name='pdm-out'),
        _i2s_record('pdm_in', 'clk', format_name='pdm-in'),
    ]
    assert is_duplicate_io_conflict(pair) is False


def test_same_field_pdm_tx_rx_same_port_dout_is_conflict() -> None:
    pair = [
        _i2s_record('pdm_out', 'dout', format_name='pdm-out'),
        _i2s_record('pdm_in', 'dout', format_name='pdm-in'),
    ]
    assert is_duplicate_io_conflict(pair) is True


def test_different_fields_same_port_tx_rx_is_conflict() -> None:
    pair = [
        _i2s_record('i2s_out', 'mclk', format_name='std-out'),
        _i2s_record('i2s_in', 'bclk', format_name='std-in'),
    ]
    assert is_duplicate_io_conflict(pair) is True


def test_same_field_same_direction_is_conflict() -> None:
    pair = [
        _i2s_record('i2s0', 'bclk', format_name='std-out'),
        _i2s_record('i2s1', 'bclk', format_name='std-out'),
    ]
    assert is_duplicate_io_conflict(pair) is True


def test_same_field_different_port_is_conflict() -> None:
    pair = [
        _i2s_record('i2s0', 'mclk', port=0, format_name='std-out'),
        _i2s_record('i2s1', 'mclk', port=1, format_name='std-in'),
    ]
    assert is_duplicate_io_conflict(pair) is True


def test_missing_format_is_conflict() -> None:
    first = _i2s_record('i2s0', 'mclk', format_name='std-out')
    second = _i2s_record('i2s1', 'mclk', format_name='std-in')
    second['artifact']['format'] = None
    assert is_duplicate_io_conflict([first, second]) is True


def test_struct_init_direction_override_is_respected() -> None:
    pair = [
        _i2s_record('i2s0', 'mclk', format_name='std-out', direction='I2S_DIR_TX'),
        _i2s_record('i2s1', 'mclk', format_name='std-out', direction='I2S_DIR_RX'),
    ]
    assert is_duplicate_io_conflict(pair) is False
