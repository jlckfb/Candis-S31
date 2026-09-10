# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

import json
import logging
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from generators.board_metadata_generator import BoardMetadataGenerator
from generators.utils import soc_capability_query as query
from generators.utils.soc_capabilities import SocCapabilityCatalog


def _configure_gpio_catalog(tmp_path: Path) -> None:
    catalog_dir = tmp_path / 'soc_capability_catalog'
    catalog_dir.mkdir()
    (catalog_dir / 'index.json').write_text(
        """
{
  "schemaVersion": 1,
  "catalogSchemaVersion": 3,
  "profiles": [
    {"id": "5.5", "version": "5.5.0", "path": "idf_5_5.json"}
  ]
}
""",
        encoding='utf-8',
    )
    catalog = SocCapabilityCatalog.from_dict({
        'schemaVersion': 3,
        'profile': {'id': '5.5'},
        'capabilityDefs': {},
        'hardwareLimitDefs': {},
        'chips': {
            'esp32s3': {
                'capabilities': {},
                'gpio': {'validInput': [0, 1, 2, 4], 'validOutput': [0, 1, 2]},
                'hardwareLimits': {},
            },
        },
    })
    (catalog_dir / 'idf_5_5.json').write_text(
        json.dumps(catalog.to_dict()),
        encoding='utf-8',
    )
    query.configure_soc_capabilities(catalog_dir=catalog_dir, idf_version='5.5.1', chip='esp32s3')


def _metadata(io_value):
    return {
        'version': 1,
        'board': 'test_board',
        'chip': 'esp32s3',
        'devices': {
            'codec': {
                'type': 'audio_codec',
                'io': {
                    'reset_gpio': io_value,
                    'nested': {'irq_gpio': 'GPIO_NUM_2'},
                },
            },
        },
        'peripherals': {
            'i2c0': {
                'type': 'i2c',
                'io': {
                    'sda_io_num': 0,
                    'scl_io_num': 'GPIO_NUM_1',
                },
            },
        },
    }


def test_metadata_io_validation_accepts_valid_soc_gpio(tmp_path: Path) -> None:
    _configure_gpio_catalog(tmp_path)

    BoardMetadataGenerator().validate_metadata_io(_metadata('GPIO_NUM_2'))


def test_metadata_io_validation_rejects_invalid_soc_gpio(tmp_path: Path) -> None:
    _configure_gpio_catalog(tmp_path)

    with pytest.raises(ValueError) as exc_info:
        BoardMetadataGenerator().validate_metadata_io(_metadata('GPIO_NUM_99'))

    message = str(exc_info.value)
    assert "device 'codec'" in message
    assert 'reset_gpio' in message
    assert 'pin 99' in message
    assert 'esp32s3' in message


def test_metadata_io_validation_checks_list_values(tmp_path: Path) -> None:
    _configure_gpio_catalog(tmp_path)

    with pytest.raises(ValueError) as exc_info:
        BoardMetadataGenerator().validate_metadata_io(_metadata([0, 'GPIO_NUM_1', 'GPIO_NUM_99']))

    message = str(exc_info.value)
    assert 'reset_gpio' in message
    assert 'pin 99' in message


def test_metadata_io_validation_skips_nc_and_io_expander_values(tmp_path: Path) -> None:
    _configure_gpio_catalog(tmp_path)
    metadata = _metadata('GPIO_NUM_NC')
    metadata['devices']['codec']['io']['nested']['irq_gpio'] = -1
    metadata['devices']['codec']['io']['cs_expander_pin'] = 99
    metadata['peripherals']['i2c0']['io']['sda_io_num'] = 'IO_EXPANDER_PIN_NUM_99'
    metadata['peripherals']['i2c0']['io']['scl_io_num'] = 'IO_EXPANDER_PIN_NUM_NC'

    BoardMetadataGenerator().validate_metadata_io(metadata)


def test_metadata_io_validation_fails_open_without_catalog() -> None:
    query.clear_soc_capabilities()

    BoardMetadataGenerator().validate_metadata_io(_metadata('GPIO_NUM_99'))


def test_write_metadata_file_validates_extracted_io_before_writing(tmp_path: Path) -> None:
    _configure_gpio_catalog(tmp_path)

    def parse_func(name, config):
        return None

    parse_func.__globals__['PERIPH_I2C_IO_LIST'] = ['sda_io_num']

    with pytest.raises(ValueError, match='sda_io_num'):
        BoardMetadataGenerator().write_metadata_file(
            output_path=str(tmp_path / 'gen_board_metadata.yaml'),
            board_name='test_board',
            chip_name='esp32s3',
            device_artifacts=[],
            peripheral_artifacts=[
                {
                    'name': 'i2c0',
                    'type': 'i2c',
                    'role': None,
                    'format': None,
                    'raw': {},
                    'result': {
                        'struct_init': {
                            'sda_io_num': 'GPIO_NUM_99',
                        },
                    },
                    'parse_func': parse_func,
                },
            ],
        )

    assert not (tmp_path / 'gen_board_metadata.yaml').exists()


def _make_device_artifact(
    name: str,
    pin_value,
    *,
    device_type: str = 'audio_codec',
    field_name: str = 'reset_gpio',
):
    def parse_func(_name, _config):
        return None

    parse_func.__globals__[f'DEV_{device_type.upper()}_IO_LIST'] = [field_name]
    return {
        'name': name,
        'type': device_type,
        'sub_type': None,
        'role': None,
        'format': None,
        'raw': {},
        'result': {
            'struct_init': {
                field_name: pin_value,
            },
        },
        'parse_func': parse_func,
    }


def _make_peripheral_artifact(
    name: str,
    pin_value,
    *,
    periph_type: str = 'i2c',
    field_name: str = 'sda_io_num',
):
    def parse_func(_name, _config):
        return None

    parse_func.__globals__[f'PERIPH_{periph_type.upper()}_IO_LIST'] = [field_name]
    return {
        'name': name,
        'type': periph_type,
        'role': None,
        'format': None,
        'raw': {},
        'result': {
            'struct_init': {
                field_name: pin_value,
            },
        },
        'parse_func': parse_func,
    }


def _make_i2s_artifact(name: str, pin_value, port: int):
    def parse_func(_name, _config):
        return None

    parse_func.__globals__['PERIPH_I2S_IO_LIST'] = {'std': ['bclk']}
    return {
        'name': name,
        'type': 'i2s',
        'role': 'master',
        'format': 'std-out',
        'raw': {'config': {'port': port}},
        'result': {
            'struct_init': {
                'i2s_cfg': {
                    'std': {
                        'gpio_cfg': {
                            'bclk': pin_value,
                        },
                    },
                },
            },
        },
        'parse_func': parse_func,
    }


def _make_i2s_artifact_with_fields(
    name: str,
    *,
    port: int,
    format_name: str,
    role: str,
    fields: dict,
    config_type: str = 'std',
):
    def parse_func(_name, _config):
        return None

    parse_func.__globals__['PERIPH_I2S_IO_LIST'] = {config_type: list(fields.keys())}
    gpio_cfg = dict(fields)
    return {
        'name': name,
        'type': 'i2s',
        'role': role,
        'format': format_name,
        'raw': {'config': {'port': port}},
        'result': {
            'struct_init': {
                'i2s_cfg': {
                    config_type: {
                        'gpio_cfg': gpio_cfg,
                    },
                },
            },
        },
        'parse_func': parse_func,
    }


def _make_sdmmc_artifact(name: str, slot: str = 'SDMMC_HOST_SLOT_0'):
    def parse_func(_name, _config):
        return None

    parse_func.__globals__['DEV_FS_FAT_IO_LIST'] = {'sdmmc': ['clk', 'cmd', 'd0', 'd1', 'd2', 'd3', 'd4', 'd5', 'd6', 'd7', 'cd', 'wp']}
    return {
        'name': name,
        'type': 'fs_fat',
        'sub_type': 'sdmmc',
        'role': None,
        'format': None,
        'raw': {
            'config': {
                'sub_config': {
                    'slot': slot,
                },
            },
        },
        'result': {
            'struct_init': {
                'sub_type': 'sdmmc',
                'sub_cfg': {
                    'sdmmc': {
                        'slot': slot,
                        'pins': {
                            'clk': 0,
                            'cmd': 0,
                            'd0': 0,
                            'd1': 0,
                            'd2': 0,
                            'd3': 0,
                            'd4': 0,
                            'd5': 0,
                            'd6': 0,
                            'd7': 0,
                            'cd': 0,
                            'wp': 0,
                        },
                    },
                },
            },
        },
        'parse_func': parse_func,
    }


def test_generate_metadata_dict_warns_on_duplicate_io(tmp_path: Path) -> None:
    _configure_gpio_catalog(tmp_path)
    generator = BoardMetadataGenerator()

    metadata = generator.generate_metadata_dict(
        board_name='test_board',
        chip_name='esp32s3',
        device_artifacts=[],
        peripheral_artifacts=[
            _make_peripheral_artifact('i2c0', 'GPIO_NUM_2'),
            _make_peripheral_artifact('i2c1', 'GPIO_NUM_2'),
        ],
    )

    assert metadata['peripherals']['i2c0']['io']['sda_io_num'] == 'GPIO_NUM_2'
    assert metadata['peripherals']['i2c1']['io']['sda_io_num'] == 'GPIO_NUM_2'
    assert generator.io_conflict_warnings == [
        'IO conflict: i2c0 (i2c).sda_io_num <-> i2c1 (i2c).sda_io_num on GPIO 2',
    ]


def test_generate_metadata_dict_skips_nc_and_negative_pins(
    tmp_path: Path,
) -> None:
    _configure_gpio_catalog(tmp_path)
    generator = BoardMetadataGenerator()

    metadata = generator.generate_metadata_dict(
        board_name='test_board',
        chip_name='esp32s3',
        device_artifacts=[],
        peripheral_artifacts=[
            _make_peripheral_artifact('i2c0', -1),
            _make_peripheral_artifact('i2c1', 'GPIO_NUM_NC'),
            _make_peripheral_artifact('i2c2', 'GPIO_NUM_2'),
        ],
    )

    assert metadata['peripherals']['i2c0'] == {'type': 'i2c'}
    assert metadata['peripherals']['i2c1'] == {'type': 'i2c'}
    assert metadata['peripherals']['i2c2']['io']['sda_io_num'] == 'GPIO_NUM_2'
    assert not generator.io_conflict_warnings


def test_generate_metadata_dict_routes_all_i2s_duplicates_through_i2s_context(
    tmp_path: Path,
) -> None:
    _configure_gpio_catalog(tmp_path)
    generator = BoardMetadataGenerator()
    metadata = generator.generate_metadata_dict(
        board_name='test_board',
        chip_name='esp32s3',
        device_artifacts=[],
        peripheral_artifacts=[
            _make_i2s_artifact('i2s0', 'GPIO_NUM_5', 0),
            _make_i2s_artifact('i2s1', 'GPIO_NUM_5', 0),
        ],
    )

    assert metadata['peripherals']['i2s0']['io']['bclk'] == 'GPIO_NUM_5'
    assert metadata['peripherals']['i2s1']['io']['bclk'] == 'GPIO_NUM_5'
    assert generator.io_conflict_warnings == [
        'IO conflict: i2s0 (i2s).bclk <-> i2s1 (i2s).bclk on GPIO 5',
    ]


def test_generate_metadata_dict_allows_supported_i2s_shared_io_without_warning(
    tmp_path: Path,
) -> None:
    _configure_gpio_catalog(tmp_path)
    generator = BoardMetadataGenerator()

    metadata = generator.generate_metadata_dict(
            board_name='test_board',
            chip_name='esp32s3',
            device_artifacts=[],
            peripheral_artifacts=[
                _make_i2s_artifact_with_fields(
                    'i2s0',
                    port=0,
                    format_name='std-out',
                    role='master',
                    fields={
                        'mclk': 'GPIO_NUM_13',
                        'bclk': 'GPIO_NUM_12',
                        'ws': 'GPIO_NUM_10',
                        'dout': 'GPIO_NUM_9',
                        'din': 'GPIO_NUM_11',
                    },
                ),
                _make_i2s_artifact_with_fields(
                    'i2s1',
                    port=0,
                    format_name='std-in',
                    role='master',
                    fields={
                        'mclk': 'GPIO_NUM_13',
                        'bclk': 'GPIO_NUM_12',
                        'ws': 'GPIO_NUM_10',
                        'dout': 'GPIO_NUM_9',
                        'din': 'GPIO_NUM_11',
                    },
                ),
            ],
    )

    assert metadata['peripherals']['i2s0']['io']['mclk'] == 'GPIO_NUM_13'
    assert metadata['peripherals']['i2s1']['io']['mclk'] == 'GPIO_NUM_13'
    assert not generator.io_conflict_warnings


def test_generate_metadata_dict_warns_on_conflicting_i2s_io(
    tmp_path: Path,
) -> None:
    _configure_gpio_catalog(tmp_path)
    generator = BoardMetadataGenerator()

    metadata = generator.generate_metadata_dict(
            board_name='test_board',
            chip_name='esp32s3',
            device_artifacts=[],
            peripheral_artifacts=[
                _make_i2s_artifact_with_fields(
                    'i2s0',
                    port=0,
                    format_name='std-out',
                    role='master',
                    fields={'mclk': 'GPIO_NUM_13', 'bclk': 'GPIO_NUM_12', 'ws': 'GPIO_NUM_10'},
                ),
                _make_i2s_artifact_with_fields(
                    'i2s1',
                    port=1,
                    format_name='std-in',
                    role='master',
                    fields={'mclk': 'GPIO_NUM_13', 'bclk': 'GPIO_NUM_12', 'ws': 'GPIO_NUM_10'},
                ),
            ],
    )

    assert metadata['peripherals']['i2s0']['io']['mclk'] == 'GPIO_NUM_13'
    assert metadata['peripherals']['i2s1']['io']['mclk'] == 'GPIO_NUM_13'
    assert generator.io_conflict_warnings == [
        'IO conflict: i2s0 (i2s).mclk <-> i2s1 (i2s).mclk on GPIO 13',
        'IO conflict: i2s0 (i2s).bclk <-> i2s1 (i2s).bclk on GPIO 12',
        'IO conflict: i2s0 (i2s).ws <-> i2s1 (i2s).ws on GPIO 10',
    ]


def test_generate_metadata_dict_allows_tdm_tx_rx_shared_io_without_warning(
    tmp_path: Path,
) -> None:
    _configure_gpio_catalog(tmp_path)
    generator = BoardMetadataGenerator()

    metadata = generator.generate_metadata_dict(
        board_name='test_board',
        chip_name='esp32s3',
        device_artifacts=[],
        peripheral_artifacts=[
            _make_i2s_artifact_with_fields(
                'tdm_out',
                port=0,
                format_name='tdm-out',
                role='master',
                config_type='tdm',
                fields={'mclk': 'GPIO_NUM_13', 'bclk': 'GPIO_NUM_12', 'ws': 'GPIO_NUM_10'},
            ),
            _make_i2s_artifact_with_fields(
                'tdm_in',
                port=0,
                format_name='tdm-in',
                role='master',
                config_type='tdm',
                fields={'mclk': 'GPIO_NUM_13', 'bclk': 'GPIO_NUM_12', 'ws': 'GPIO_NUM_10'},
            ),
        ],
    )

    assert metadata['peripherals']['tdm_out']['io']['mclk'] == 'GPIO_NUM_13'
    assert not generator.io_conflict_warnings


def test_generate_metadata_dict_warns_on_cross_field_i2s_duplicate(
    tmp_path: Path,
) -> None:
    _configure_gpio_catalog(tmp_path)
    generator = BoardMetadataGenerator()

    generator.generate_metadata_dict(
        board_name='test_board',
        chip_name='esp32s3',
        device_artifacts=[],
        peripheral_artifacts=[
            _make_i2s_artifact_with_fields(
                'i2s_out',
                port=0,
                format_name='std-out',
                role='master',
                fields={'mclk': 'GPIO_NUM_13', 'bclk': 'GPIO_NUM_12'},
            ),
            _make_i2s_artifact_with_fields(
                'i2s_in',
                port=0,
                format_name='std-in',
                role='master',
                fields={'mclk': 'GPIO_NUM_12', 'bclk': 'GPIO_NUM_13'},
            ),
        ],
    )

    assert generator.io_conflict_warnings == [
        'IO conflict: i2s_out (i2s).mclk <-> i2s_in (i2s).bclk on GPIO 13',
        'IO conflict: i2s_out (i2s).bclk <-> i2s_in (i2s).mclk on GPIO 12',
    ]


def test_generate_metadata_dict_warns_on_device_peripheral_duplicate_io(
    tmp_path: Path,
) -> None:
    _configure_gpio_catalog(tmp_path)
    generator = BoardMetadataGenerator()

    metadata = generator.generate_metadata_dict(
        board_name='test_board',
        chip_name='esp32s3',
        device_artifacts=[
            _make_device_artifact('codec', 'GPIO_NUM_2', field_name='reset_gpio'),
        ],
        peripheral_artifacts=[
            _make_peripheral_artifact('i2c0', 'GPIO_NUM_2'),
        ],
    )

    assert metadata['devices']['codec']['io']['reset_gpio'] == 'GPIO_NUM_2'
    assert metadata['peripherals']['i2c0']['io']['sda_io_num'] == 'GPIO_NUM_2'
    assert generator.io_conflict_warnings == [
        'IO conflict: codec (audio_codec).reset_gpio <-> i2c0 (i2c).sda_io_num on GPIO 2',
    ]


def test_generate_metadata_dict_warns_on_device_device_duplicate_io(
    tmp_path: Path,
) -> None:
    _configure_gpio_catalog(tmp_path)
    generator = BoardMetadataGenerator()

    generator.generate_metadata_dict(
        board_name='test_board',
        chip_name='esp32s3',
        device_artifacts=[
            _make_device_artifact('codec0', 'GPIO_NUM_2', field_name='reset_gpio'),
            _make_device_artifact('codec1', 'GPIO_NUM_2', field_name='reset_gpio'),
        ],
        peripheral_artifacts=[],
    )

    assert generator.io_conflict_warnings == [
        'IO conflict: codec0 (audio_codec).reset_gpio <-> codec1 (audio_codec).reset_gpio on GPIO 2',
    ]


def test_generate_metadata_dict_prefers_non_i2s_owner_for_mixed_duplicates(
    tmp_path: Path,
) -> None:
    _configure_gpio_catalog(tmp_path)
    generator = BoardMetadataGenerator()

    generator.generate_metadata_dict(
            board_name='test_board',
            chip_name='esp32s3',
            device_artifacts=[],
            peripheral_artifacts=[
                _make_i2s_artifact_with_fields(
                    'i2s0',
                    port=0,
                    format_name='std-out',
                    role='master',
                    fields={
                        'mclk': 'GPIO_NUM_13',
                        'bclk': 'GPIO_NUM_12',
                        'ws': 'GPIO_NUM_10',
                        'dout': 'GPIO_NUM_9',
                        'din': 'GPIO_NUM_11',
                    },
                ),
                _make_i2s_artifact_with_fields(
                    'i2s1',
                    port=0,
                    format_name='std-in',
                    role='master',
                    fields={
                        'mclk': 'GPIO_NUM_13',
                        'bclk': 'GPIO_NUM_12',
                        'ws': 'GPIO_NUM_10',
                        'dout': 'GPIO_NUM_9',
                        'din': 'GPIO_NUM_11',
                    },
                ),
                _make_peripheral_artifact('gpio_pa_control', 13, periph_type='gpio', field_name='pin'),
            ],
    )

    assert any(
        message == 'IO conflict: gpio_pa_control (gpio).pin <-> i2s0 (i2s).mclk on GPIO 13'
        for message in generator.io_conflict_warnings
    )
    assert any(
        message == 'IO conflict: gpio_pa_control (gpio).pin <-> i2s1 (i2s).mclk on GPIO 13'
        for message in generator.io_conflict_warnings
    )


def test_report_io_conflict_warnings_prints_grouped_summary(
    tmp_path: Path,
    caplog: pytest.LogCaptureFixture,
) -> None:
    _configure_gpio_catalog(tmp_path)
    generator = BoardMetadataGenerator()
    generator.generate_metadata_dict(
        board_name='test_board',
        chip_name='esp32s3',
        device_artifacts=[],
        peripheral_artifacts=[
            _make_peripheral_artifact('i2c0', 'GPIO_NUM_2'),
            _make_peripheral_artifact('i2c1', 'GPIO_NUM_2'),
        ],
    )

    with caplog.at_level(logging.WARNING):
        count = generator.report_io_conflict_warnings()

    assert count == 1
    messages = [record.message for record in caplog.records]
    assert any('IO conflict warnings (1)' in message for message in messages)
    assert any('[1] IO conflict: i2c0 (i2c).sda_io_num <-> i2c1 (i2c).sda_io_num on GPIO 2' in message for message in messages)


def test_generate_metadata_dict_skips_sdmmc_slot0_placeholder_pins(
    tmp_path: Path,
) -> None:
    _configure_gpio_catalog(tmp_path)
    generator = BoardMetadataGenerator()

    generator.generate_metadata_dict(
            board_name='test_board',
            chip_name='esp32s3',
            device_artifacts=[
                _make_sdmmc_artifact('fs_sdcard'),
            ],
            peripheral_artifacts=[],
    )

    assert not generator.io_conflict_warnings
