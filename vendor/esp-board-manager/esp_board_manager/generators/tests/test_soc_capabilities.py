# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from generators.utils.soc_capabilities import (
    CapabilityMatch,
    HardwareLimitApplies,
    SocCapabilityCatalog,
    SocCapabilityProvider,
    SocCapsParser,
    load_soc_capability_spec,
    load_soc_requirement_rules,
)
from export_soc_capability_catalog import _parse_args, export_soc_capability_catalog


def _write_soc_caps(root: Path, chip: str, text: str) -> None:
    path = root / 'components' / 'soc' / chip / 'include' / 'soc' / 'soc_caps.h'
    path.parent.mkdir(parents=True)
    path.write_text(text, encoding='utf-8')


def _write_adc_channel_header(root: Path, chip: str, text: str) -> None:
    path = root / 'components' / 'soc' / chip / 'include' / 'soc' / 'adc_channel.h'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8')


def _write_idf_file(root: Path, relative: str, text: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True)
    path.write_text(text, encoding='utf-8')


def test_capability_match_from_yaml_and_to_dict() -> None:
    match = CapabilityMatch.from_yaml(
        'peripheral',
        {'type': 'i2s', 'role': 'master', 'format': ['std-out', 'std-in']},
    )
    assert match.kind == 'peripheral'
    assert match.type == 'i2s'
    assert match.selectors == {'role': ['master'], 'format': ['std-out', 'std-in']}
    assert match.to_dict() == {
        'kind': 'peripheral',
        'type': 'i2s',
        'format': ['std-out', 'std-in'],
        'role': ['master'],
    }


def test_hardware_limit_applies_to_dict() -> None:
    applies = HardwareLimitApplies(
        kind='device',
        type='display_lcd',
        selectors={'sub_type': ['rgb']},
        path=['data_width'],
        check='value',
    )
    assert applies.to_dict() == {
        'kind': 'device',
        'type': 'display_lcd',
        'sub_type': ['rgb'],
        'path': ['data_width'],
        'check': 'value',
    }


def test_hardware_limit_applies_from_inline_yaml() -> None:
    applies = HardwareLimitApplies.from_yaml(
        {'kind': 'device', 'type': 'display_lcd', 'sub_type': ['rgb'], 'path': ['data_width'], 'check': 'value'}
    )
    assert applies.selectors == {'sub_type': ['rgb']}
    assert applies.path == ['data_width']
    assert applies.check == 'value'


def test_load_spec_parses_match_and_applies(tmp_path: Path) -> None:
    req = tmp_path / 'req.yml'
    req.write_text(
        """
        devices:
          - display_lcd_parlio:
              allOf: [SOC_PARLIO_SUPPORTED]
              anyOf: [SOC_PARLIO_LCD_SUPPORTED]
              match: { type: display_lcd, sub_type: parlio }
          - gpio_expander:
              requires: SOC_I2C_SUPPORTED
              match: { type: gpio_expander }
        peripherals:
          - i2c: SOC_I2C_SUPPORTED
          - i2s_master_std:
              requires: SOC_I2S_SUPPORTED
              match: { type: i2s, role: master, format: [std-out, std-in] }
        hardware_limits:
          lcd.rgb_data_width:
            sources: [{ kind: soc_caps_macro, symbol: SOC_LCDCAM_RGB_DATA_WIDTH }]
            applies_to:
              - { kind: device, type: display_lcd, sub_type: [rgb], path: [data_width], check: value }
        """,
        encoding='utf-8',
    )

    spec = load_soc_capability_spec(req)

    assert spec.requirement_rules['devices']['display_lcd_parlio'].all_of == ['SOC_PARLIO_SUPPORTED']
    assert spec.requirement_rules['devices']['gpio_expander'].all_of == ['SOC_I2C_SUPPORTED']
    assert spec.requirement_rules['peripherals']['i2c'].all_of == ['SOC_I2C_SUPPORTED']
    assert spec.capability_matches['devices']['display_lcd_parlio'].selectors == {'sub_type': ['parlio']}
    assert spec.capability_matches['devices']['gpio_expander'].selectors == {}
    assert spec.capability_matches['peripherals']['i2s_master_std'].selectors['format'] == ['std-out', 'std-in']
    assert spec.hardware_limits['lcd.rgb_data_width'].applies_to[0].check == 'value'
    assert spec.hardware_limits['lcd.rgb_data_width'].applies_to[0].selectors == {'sub_type': ['rgb']}


def _build_v2_catalog(tmp_path: Path) -> SocCapabilityCatalog:
    req = tmp_path / 'req.yml'
    req.write_text(
        """
        peripherals:
          - i2s_master_std:
              requires: SOC_I2S_SUPPORTED
              match: { type: i2s, role: master, format: [std-out, std-in] }
          - i2c:
              requires: SOC_I2C_SUPPORTED
              match: { type: i2c }
        hardware_limits:
          lcd.rgb_data_width:
            sources: [{ kind: soc_caps_macro, symbol: SOC_LCDCAM_RGB_DATA_WIDTH }]
            applies_to:
              - { kind: device, type: display_lcd, sub_type: [rgb], path: [data_width], check: value }
        """,
        encoding='utf-8',
    )
    idf = tmp_path / 'idf'
    _write_soc_caps(
        idf,
        'esp32s3',
        """
        #define SOC_I2S_SUPPORTED 1
        #define SOC_I2C_SUPPORTED 1
        #define SOC_LCDCAM_RGB_DATA_WIDTH 16
        """,
    )
    return SocCapabilityCatalog.build(req, [('5.5', idf)], ['esp32s3'])


def test_to_dict_emits_defs_and_schema_v3(tmp_path: Path) -> None:
    catalog = _build_v2_catalog(tmp_path)
    data = catalog.to_dict()

    assert data['schemaVersion'] == 3
    assert data['profile'] == {'id': '5.5'}
    assert data['capabilityDefs']['peripheral.i2s_master_std'] == {
        'kind': 'peripheral',
        'type': 'i2s',
        'format': ['std-out', 'std-in'],
        'role': ['master'],
    }
    assert data['capabilityDefs']['peripheral.i2c'] == {'kind': 'peripheral', 'type': 'i2c'}
    assert data['hardwareLimitDefs']['lcd.rgb_data_width']['appliesTo'][0] == {
        'kind': 'device',
        'type': 'display_lcd',
        'sub_type': ['rgb'],
        'path': ['data_width'],
        'check': 'value',
    }
    chip_data = next(iter(data['chips'].values()))
    assert 'capabilityDefs' not in chip_data
    assert 'hardwareLimitDefs' not in chip_data
    assert 'supportedSince' not in chip_data
    assert catalog.diagnostics == []


def test_to_dict_reports_capability_without_match(tmp_path: Path) -> None:
    req = tmp_path / 'req.yml'
    req.write_text(
        """
        peripherals:
          - i2c: SOC_I2C_SUPPORTED
        """,
        encoding='utf-8',
    )
    idf = tmp_path / 'idf'
    _write_soc_caps(idf, 'esp32s3', '#define SOC_I2C_SUPPORTED 1\n')

    catalog = SocCapabilityCatalog.build(req, [('5.5', idf)], ['esp32s3'])
    data = catalog.to_dict()

    assert 'peripheral.i2c' not in data['capabilityDefs']
    assert 'peripheral.i2c: capability has no web match spec' in catalog.diagnostics


def test_from_dict_roundtrip_v3(tmp_path: Path) -> None:
    data = _build_v2_catalog(tmp_path).to_dict()
    again = SocCapabilityCatalog.from_dict(data).to_dict()

    assert again['schemaVersion'] == 3
    assert again['capabilityDefs'] == data['capabilityDefs']
    assert again['hardwareLimitDefs'] == data['hardwareLimitDefs']


def test_from_dict_reads_v1_catalog_without_defs(tmp_path: Path) -> None:
    catalog_path = tmp_path / 'catalog.json'
    catalog_path.write_text(
        """
        {
          "schemaVersion": 1,
          "generatedFromProfiles": [{"id": "5.5"}],
          "chips": {
            "esp32s3": {
              "supportedSince": "5.5",
              "capabilities": {"peripheral.i2c": true},
              "hardwareLimits": {"i2c.instance_count": 2},
              "gpio": {"validInput": [0, 1], "validOutput": [0], "inputRangeMax": 1, "outputRangeMax": 0}
            }
          }
        }
        """,
        encoding='utf-8',
    )

    catalog = SocCapabilityCatalog.from_dict(json.loads(catalog_path.read_text(encoding='utf-8')))
    data = catalog.to_dict()

    assert catalog.chip('esp32s3').supports('peripheral.i2c')
    assert data['capabilityDefs'] == {}
    assert data['hardwareLimitDefs'] == {}


def test_soc_caps_parser_extracts_booleans_numbers_and_gpio_masks(tmp_path: Path) -> None:
    idf = tmp_path / 'idf'
    _write_soc_caps(
        idf,
        'esp32s3',
        """
        #define SOC_I2C_SUPPORTED 1
        #define SOC_I2C_NUM (2U)
        #define SOC_GPIO_PIN_COUNT 8
        #define SOC_GPIO_VALID_GPIO_MASK ((1ULL << SOC_GPIO_PIN_COUNT) - 1)
        #define SOC_GPIO_VALID_OUTPUT_GPIO_MASK (SOC_GPIO_VALID_GPIO_MASK & ~(0ULL | BIT6 | BIT7))
        #define SOC_GPIO_IN_RANGE_MAX 7
        #define SOC_GPIO_OUT_RANGE_MAX 5
        """,
    )

    profile = SocCapsParser(idf).parse_chip('esp32s3')

    assert profile.booleans['SOC_I2C_SUPPORTED'] is True
    assert profile.numbers['SOC_I2C_NUM'] == 2
    assert profile.gpio.valid_input == list(range(8))
    assert profile.gpio.valid_output == [0, 1, 2, 3, 4, 5]
    assert profile.gpio.input_range_max == 7
    assert profile.gpio.output_range_max == 5


def test_catalog_build_exports_adc_channel_map(tmp_path: Path) -> None:
    req = tmp_path / 'esp_board_soc_requirements.yml'
    req.write_text(
        """
        peripherals:
          - adc_oneshot:
              requires: SOC_ADC_SUPPORTED
              match: { type: adc, role: oneshot }
        """,
        encoding='utf-8',
    )
    idf = tmp_path / 'idf'
    _write_soc_caps(idf, 'esp32s3', '#define SOC_ADC_SUPPORTED 1\n')
    _write_adc_channel_header(
        idf,
        'esp32s3',
        '#define ADC1_CHANNEL_0_GPIO_NUM 36\n#define ADC1_CHANNEL_4_GPIO_NUM 32\n',
    )

    catalog = SocCapabilityCatalog.build(req, [('5.5', idf)], ['esp32s3'])
    chip_caps = catalog.chip('esp32s3')

    assert chip_caps.adc_channel_map == {1: {0: 36, 4: 32}}
    exported = chip_caps.to_dict()['adcChannelMap']
    assert exported == {'1': {'0': 36, '4': 32}}


def test_i2c_lp_capability_and_limit_are_exported(tmp_path: Path) -> None:
    req = tmp_path / 'esp_board_soc_requirements.yml'
    req.write_text(
        """
        capabilities:
          i2c.lp_supported: SOC_LP_I2C_SUPPORTED
        hardware_limits:
          i2c.lp_instance_count:
            sources:
              - kind: soc_caps_macro
                symbol: SOC_LP_I2C_NUM
        """,
        encoding='utf-8',
    )
    idf = tmp_path / 'idf'
    _write_soc_caps(
        idf,
        'esp32c5',
        """
        #define SOC_LP_I2C_SUPPORTED 1
        #define SOC_LP_I2C_NUM (1U)
        """,
    )

    catalog = SocCapabilityCatalog.build(req, [('6.1', idf)], ['esp32c5'])
    chip_caps = catalog.chip('esp32c5')

    assert chip_caps.supports('i2c.lp_supported') is True
    assert chip_caps.hardware_limit('i2c.lp_instance_count') == 1


def test_requirement_rules_support_simple_and_allof_anyof_forms(tmp_path: Path) -> None:
    req = tmp_path / 'esp_board_soc_requirements.yml'
    req.write_text(
        """
        devices:
          - display_lcd_dsi: SOC_MIPI_DSI_SUPPORTED
          - display_lcd_parlio:
              allOf:
                - SOC_PARLIO_SUPPORTED
              anyOf:
                - SOC_PARLIO_LCD_SUPPORTED
                - SOC_PARLIO_SUPPORT_SPI_LCD
                - SOC_PARLIO_SUPPORT_I80_LCD
        peripherals:
          - i2c: SOC_I2C_SUPPORTED
        """,
        encoding='utf-8',
    )

    rules = load_soc_requirement_rules(req)

    assert rules['devices']['display_lcd_dsi'].all_of == ['SOC_MIPI_DSI_SUPPORTED']
    assert rules['devices']['display_lcd_parlio'].all_of == ['SOC_PARLIO_SUPPORTED']
    assert rules['devices']['display_lcd_parlio'].any_of == [
        'SOC_PARLIO_LCD_SUPPORTED',
        'SOC_PARLIO_SUPPORT_SPI_LCD',
        'SOC_PARLIO_SUPPORT_I80_LCD',
    ]
    assert rules['peripherals']['i2c'].all_of == ['SOC_I2C_SUPPORTED']


def test_catalog_preserves_profile_differences_for_each_export_file(tmp_path: Path) -> None:
    req = tmp_path / 'esp_board_soc_requirements.yml'
    req.write_text(
        """
        devices:
          - display_lcd_parlio:
              allOf:
                - SOC_PARLIO_SUPPORTED
              anyOf:
                - SOC_PARLIO_LCD_SUPPORTED
                - SOC_PARLIO_SUPPORT_SPI_LCD
                - SOC_PARLIO_SUPPORT_I80_LCD
        peripherals:
          - i2c: SOC_I2C_SUPPORTED
        hardware_limits:
          i2s.instance_count:
            sources:
              - kind: soc_caps_macro
                symbol: SOC_I2S_NUM
        """,
        encoding='utf-8',
    )
    idf55 = tmp_path / 'idf55'
    idf62 = tmp_path / 'idf62'
    _write_soc_caps(
        idf55,
        'esp32p4',
        """
        #define SOC_PARLIO_SUPPORTED 1
        #define SOC_PARLIO_SUPPORT_SPI_LCD 1
        #define SOC_I2C_SUPPORTED 1
        #define SOC_I2S_NUM (2U)
        """,
    )
    _write_soc_caps(
        idf62,
        'esp32p4',
        """
        #define SOC_PARLIO_SUPPORTED 1
        #define SOC_PARLIO_LCD_SUPPORTED 1
        #define SOC_I2C_SUPPORTED 1
        #define SOC_I2S_NUM (2U)
        """,
    )

    catalog = SocCapabilityCatalog.build(
        requirement_path=req,
        idf_profiles=[
            ('5.5', idf55),
            ('6.2', idf62),
        ],
        chips=['esp32p4'],
    )

    data55 = catalog.to_profile_dict('5.5')
    data62 = catalog.to_profile_dict('6.2')

    assert data55['schemaVersion'] == 3
    assert data62['schemaVersion'] == 3
    assert data55['profile'] == {'id': '5.5'}
    assert data62['profile'] == {'id': '6.2'}
    assert data55['chips']['esp32p4']['capabilities']['device.display_lcd_parlio'] is True
    assert data62['chips']['esp32p4']['capabilities']['device.display_lcd_parlio'] is True
    assert data55['chips']['esp32p4']['capabilities']['peripheral.i2c'] is True
    assert data55['chips']['esp32p4']['hardwareLimits']['i2s.instance_count'] == 2
    assert 'profiles' not in data55['chips']['esp32p4']
    assert 'booleans' not in data55['chips']['esp32p4']
    assert 'numbers' not in data55['chips']['esp32p4']
    assert 'sourceMacros' not in data55['chips']['esp32p4']
    assert 'rawMasks' not in data55['chips']['esp32p4']['gpio']
    assert 'sourceSpec' not in data55
    assert 'capabilityRules' not in data55


def test_catalog_serializes_one_file_per_idf_profile(tmp_path: Path) -> None:
    req = tmp_path / 'esp_board_soc_requirements.yml'
    req.write_text(
        """
        devices:
          - display_lcd_parlio:
              requires: SOC_PARLIO_SUPPORTED
              anyOf: [SOC_PARLIO_LCD_SUPPORTED, SOC_PARLIO_SUPPORT_SPI_LCD]
              match: { type: display_lcd, sub_type: parlio }
        peripherals:
          - i2c:
              requires: SOC_I2C_SUPPORTED
              match: { type: i2c }
        """,
        encoding='utf-8',
    )
    idf54 = tmp_path / 'idf54'
    idf55 = tmp_path / 'idf55'
    _write_soc_caps(idf54, 'esp32p4', '#define SOC_I2C_SUPPORTED 1\n')
    _write_soc_caps(
        idf55,
        'esp32p4',
        """
        #define SOC_I2C_SUPPORTED 1
        #define SOC_PARLIO_SUPPORTED 1
        #define SOC_PARLIO_LCD_SUPPORTED 1
        """,
    )

    catalog = SocCapabilityCatalog.build(
        requirement_path=req,
        idf_profiles=[('5.4', idf54), ('5.5', idf55)],
        chips=['esp32p4'],
    )

    data54 = catalog.to_profile_dict('5.4')
    data55 = catalog.to_profile_dict('5.5')

    assert data54['schemaVersion'] == 3
    assert data54['profile'] == {'id': '5.4'}
    assert data54['chips']['esp32p4']['capabilities']['device.display_lcd_parlio'] is False
    assert data55['chips']['esp32p4']['capabilities']['device.display_lcd_parlio'] is True
    assert data54['capabilityDefs'] == data55['capabilityDefs']
    assert 'supportedSince' not in data54['chips']['esp32p4']
    assert 'profiles' not in data54['chips']['esp32p4']
    assert 'booleans' not in data54['chips']['esp32p4']
    assert 'sourceMacros' not in data54['chips']['esp32p4']


def test_catalog_index_lists_profile_files(tmp_path: Path) -> None:
    catalog = SocCapabilityCatalog(
        profiles={},
        idf_profiles=[{'id': '5.4'}, {'id': '5.5'}, {'id': '6.x'}],
    )

    index = catalog.to_index_dict()

    assert index == {
        'schemaVersion': 1,
        'catalogSchemaVersion': 3,
        'profiles': [
            {'id': '5.4', 'version': '5.4.0', 'path': 'idf_5_4.json'},
            {'id': '5.5', 'version': '5.5.0', 'path': 'idf_5_5.json'},
            {'id': '6.x', 'version': '6.0.0', 'path': 'idf_6_x.json'},
        ],
    }


def test_select_profile_for_idf_version_uses_highest_floor() -> None:
    index = {
        'schemaVersion': 1,
        'catalogSchemaVersion': 3,
        'profiles': [
            {'id': '5.4', 'version': '5.4.0', 'path': 'idf_5_4.json'},
            {'id': '5.5', 'version': '5.5.0', 'path': 'idf_5_5.json'},
            {'id': '6.x', 'version': '6.0.0', 'path': 'idf_6_x.json'},
        ],
    }

    assert SocCapabilityCatalog.select_profile_entry(index, '5.5.4')['path'] == 'idf_5_5.json'
    assert SocCapabilityCatalog.select_profile_entry(index, '6.0.1')['path'] == 'idf_6_x.json'

    try:
        SocCapabilityCatalog.select_profile_entry(index, '5.3.2')
    except ValueError as exc:
        assert 'no SoC capability catalog profile for ESP-IDF 5.3.2' in str(exc)
    else:
        raise AssertionError('expected IDF lower than first profile to fail')


def test_catalog_derives_hardware_limits_from_ll_hal_sources(tmp_path: Path) -> None:
    req = tmp_path / 'esp_board_soc_requirements.yml'
    req.write_text(
        """
        hardware_limits:
          i2s.instance_count:
            sources:
              - kind: soc_caps_macro
                symbol: SOC_I2S_NUM
              - kind: header_define
                path: components/esp_hal_i2s/{chip}/include/hal/i2s_ll.h
                symbol: I2S_LL_INST_NUM
          mcpwm.group_count:
            sources:
              - kind: header_define
                path: components/esp_hal_mcpwm/{chip}/include/hal/mcpwm_ll.h
                symbol: MCPWM_LL_GROUP_NUM
          pcnt.channels_per_unit:
            sources:
              - kind: header_define
                path: components/esp_hal_pcnt/{chip}/include/hal/pcnt_ll.h
                symbol: PCNT_LL_CHANS_PER_UNIT
          lcd.rgb_data_width:
            sources:
              - kind: header_define
                path: components/esp_hal_lcd/{chip}/include/hal/lcd_ll.h
                symbol: LCD_LL_RGB_BUS_WIDTH
        """,
        encoding='utf-8',
    )
    idf = tmp_path / 'idf'
    _write_soc_caps(idf, 'esp32s3', '#define SOC_I2S_SUPPORTED 1\n')
    _write_idf_file(idf, 'components/esp_hal_i2s/esp32s3/include/hal/i2s_ll.h', '#define I2S_LL_INST_NUM 2\n')
    _write_idf_file(
        idf,
        'components/esp_hal_mcpwm/esp32s3/include/hal/mcpwm_ll.h',
        '#define MCPWM_LL_GROUP_NUM (2)\n',
    )
    _write_idf_file(
        idf,
        'components/esp_hal_pcnt/esp32s3/include/hal/pcnt_ll.h',
        '#define PCNT_LL_CHANS_PER_UNIT 2\n',
    )
    _write_idf_file(
        idf,
        'components/esp_hal_lcd/esp32s3/include/hal/lcd_ll.h',
        '#define LCD_LL_RGB_BUS_WIDTH 16\n',
    )

    catalog = SocCapabilityCatalog.build(req, [('6.1', idf)], ['esp32s3'])
    chip_caps = catalog.chip('esp32s3')

    assert chip_caps.hardware_limit('i2s.instance_count') == 2
    assert chip_caps.hardware_limit('mcpwm.group_count') == 2
    assert chip_caps.hardware_limit('pcnt.channels_per_unit') == 2
    assert chip_caps.hardware_limit('lcd.rgb_data_width') == 16


def test_catalog_supported_since_override_does_not_change_profile_facts(tmp_path: Path) -> None:
    req = tmp_path / 'esp_board_soc_requirements.yml'
    req.write_text(
        """
        supported_since_overrides:
          esp32h4: "6.1"
        peripherals:
          - i2c: SOC_I2C_SUPPORTED
        """,
        encoding='utf-8',
    )
    idf55 = tmp_path / 'idf55'
    idf61 = tmp_path / 'idf61'
    _write_soc_caps(idf55, 'esp32h4', '')
    _write_soc_caps(idf61, 'esp32h4', '#define SOC_I2C_SUPPORTED 1\n')

    catalog = SocCapabilityCatalog.build(req, [('5.5', idf55), ('6.1', idf61)], ['esp32h4'])
    data55 = catalog.to_profile_dict('5.5')
    data61 = catalog.to_profile_dict('6.1')

    assert data55['chips']['esp32h4']['capabilities']['peripheral.i2c'] is False
    assert data61['chips']['esp32h4']['capabilities']['peripheral.i2c'] is True
    assert 'supportedSince' not in data55['chips']['esp32h4']
    assert not hasattr(catalog, 'supported_since_overrides')


def test_provider_loads_profile_catalog_for_idf_version(tmp_path: Path) -> None:
    catalog_dir = tmp_path / 'soc_capability_catalog'
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
        'capabilityDefs': {'peripheral.i2c': {'kind': 'peripheral', 'type': 'i2c'}},
        'hardwareLimitDefs': {},
    }
    (catalog_dir / 'idf_5_4.json').write_text(
        json.dumps({
            **common,
            'profile': {'id': '5.4'},
            'chips': {'esp32s3': {'capabilities': {'peripheral.i2c': False}, 'gpio': {}, 'hardwareLimits': {}}},
        }),
        encoding='utf-8',
    )
    (catalog_dir / 'idf_5_5.json').write_text(
        json.dumps({
            **common,
            'profile': {'id': '5.5'},
            'chips': {'esp32s3': {'capabilities': {'peripheral.i2c': True}, 'gpio': {}, 'hardwareLimits': {}}},
        }),
        encoding='utf-8',
    )

    provider54 = SocCapabilityProvider.load_for_idf_version(catalog_dir, '5.4.4')
    provider55 = SocCapabilityProvider.load_for_idf_version(catalog_dir, '5.5.2')

    assert provider54.chip('esp32s3').supports('peripheral.i2c') is False
    assert provider55.chip('esp32s3').supports('peripheral.i2c') is True
    assert provider55.selected_profile_id == '5.5'
    assert not hasattr(SocCapabilityProvider, 'load')


def test_catalog_ignores_missing_supported_since_override_profile(tmp_path: Path) -> None:
    req = tmp_path / 'esp_board_soc_requirements.yml'
    req.write_text(
        """
        supported_since_overrides:
          esp32h4: "6.1"
        peripherals:
          - i2c: SOC_I2C_SUPPORTED
        """,
        encoding='utf-8',
    )
    idf55 = tmp_path / 'idf55'
    _write_soc_caps(idf55, 'esp32h4', '#define SOC_I2C_SUPPORTED 1\n')

    catalog = SocCapabilityCatalog.build(req, [('5.5', idf55)], ['esp32h4'])
    chip_data = catalog.to_profile_dict('5.5')['chips']['esp32h4']

    assert chip_data['capabilities']['peripheral.i2c'] is True
    assert 'supportedSince' not in chip_data
    assert catalog.diagnostics == ['peripheral.i2c: capability has no web match spec']
    assert not hasattr(catalog, 'supported_since_overrides')


def test_export_soc_capability_catalog_writes_index_and_profile_files(tmp_path: Path) -> None:
    req = tmp_path / 'esp_board_soc_requirements.yml'
    req.write_text(
        """
        peripherals:
          - i2c:
              requires: SOC_I2C_SUPPORTED
              match: { type: i2c }
        """,
        encoding='utf-8',
    )
    idf54 = tmp_path / 'idf54'
    idf55 = tmp_path / 'idf55'
    out_dir = tmp_path / 'soc_capability_catalog'
    _write_soc_caps(idf54, 'esp32s3', '')
    _write_soc_caps(idf55, 'esp32s3', '#define SOC_I2C_SUPPORTED 1\n')

    export_soc_capability_catalog(
        requirement_path=req,
        idf_profiles=[('5.4', idf54), ('5.5', idf55)],
        chips=['esp32s3'],
        output_path=out_dir,
    )

    index = json.loads((out_dir / 'index.json').read_text(encoding='utf-8'))
    data54 = json.loads((out_dir / 'idf_5_4.json').read_text(encoding='utf-8'))
    data55 = json.loads((out_dir / 'idf_5_5.json').read_text(encoding='utf-8'))

    assert index['catalogSchemaVersion'] == 3
    assert [entry['path'] for entry in index['profiles']] == ['idf_5_4.json', 'idf_5_5.json']
    assert data54['chips']['esp32s3']['capabilities']['peripheral.i2c'] is False
    assert data55['chips']['esp32s3']['capabilities']['peripheral.i2c'] is True


def test_export_soc_capability_catalog_strict_allows_profile_differences(tmp_path: Path) -> None:
    req = tmp_path / 'esp_board_soc_requirements.yml'
    req.write_text(
        """
        peripherals:
          - i2c:
              requires: SOC_I2C_SUPPORTED
              match: { type: i2c }
        """,
        encoding='utf-8',
    )
    idf54 = tmp_path / 'idf54'
    idf55 = tmp_path / 'idf55'
    out_dir = tmp_path / 'soc_capability_catalog'
    _write_soc_caps(idf54, 'esp32s3', '')
    _write_soc_caps(idf55, 'esp32s3', '#define SOC_I2C_SUPPORTED 1\n')

    export_soc_capability_catalog(
        requirement_path=req,
        idf_profiles=[('5.4', idf54), ('5.5', idf55)],
        chips=['esp32s3'],
        output_path=out_dir,
        strict=True,
    )

    assert (out_dir / 'index.json').is_file()
    assert (out_dir / 'idf_5_4.json').is_file()
    assert (out_dir / 'idf_5_5.json').is_file()


def test_export_soc_capability_catalog_discovers_chips_when_unspecified(tmp_path: Path) -> None:
    req = tmp_path / 'esp_board_soc_requirements.yml'
    req.write_text(
        """
        peripherals:
          - i2c: SOC_I2C_SUPPORTED
        """,
        encoding='utf-8',
    )
    idf = tmp_path / 'idf'
    out = tmp_path / 'soc_capability_catalog'
    _write_soc_caps(idf, 'esp32s3', '#define SOC_I2C_SUPPORTED 1\n')
    _write_soc_caps(idf, 'esp32c3', '#define SOC_I2C_SUPPORTED 1\n')

    export_soc_capability_catalog(
        requirement_path=req,
        idf_profiles=[('5.5', idf)],
        chips=[],
        output_path=out,
    )

    data = json.loads((out / 'idf_5_5.json').read_text(encoding='utf-8'))
    assert set(data['chips']) == {'esp32c3', 'esp32s3'}


def test_export_soc_capability_catalog_skips_chip_missing_from_one_profile(tmp_path: Path) -> None:
    req = tmp_path / 'esp_board_soc_requirements.yml'
    req.write_text(
        """
        peripherals:
          - i2c: SOC_I2C_SUPPORTED
        """,
        encoding='utf-8',
    )
    idf55 = tmp_path / 'idf55'
    idf61 = tmp_path / 'idf61'
    out = tmp_path / 'soc_capability_catalog'
    _write_soc_caps(idf55, 'esp32s3', '#define SOC_I2C_SUPPORTED 1\n')
    _write_soc_caps(idf61, 'esp32s3', '#define SOC_I2C_SUPPORTED 1\n')
    _write_soc_caps(idf61, 'esp32s31', '#define SOC_I2C_SUPPORTED 1\n')

    export_soc_capability_catalog(
        requirement_path=req,
        idf_profiles=[('5.5', idf55), ('6.1', idf61)],
        chips=[],
        output_path=out,
    )

    data55 = json.loads((out / 'idf_5_5.json').read_text(encoding='utf-8'))
    data61 = json.loads((out / 'idf_6_1.json').read_text(encoding='utf-8'))
    assert set(data55['chips']) == {'esp32s3'}
    assert set(data61['chips']) == {'esp32s3', 'esp32s31'}
    assert 'profiles' not in data55['chips']['esp32s3']
    assert 'profiles' not in data61['chips']['esp32s31']


def test_export_soc_capability_catalog_cli_allows_omitting_chip(tmp_path: Path) -> None:
    args = _parse_args([
        '--idf-profile',
        f'5.5={tmp_path / "idf"}',
    ])

    assert args.chip == []


def test_export_soc_capability_catalog_cli_accepts_strict(tmp_path: Path) -> None:
    args = _parse_args([
        '--idf-profile',
        f'5.5={tmp_path / "idf"}',
        '--strict',
    ])

    assert args.strict is True


def test_repo_soc_requirements_include_first_phase_capability_scope() -> None:
    req = Path(__file__).parent.parent.parent / 'private_inc' / 'esp_board_soc_requirements.yml'

    spec = load_soc_capability_spec(req)
    rules = spec.requirement_rules

    assert rules['devices']['display_lcd_parlio'].all_of == ['SOC_PARLIO_SUPPORTED']
    assert rules['devices']['display_lcd_parlio'].any_of == [
        'SOC_PARLIO_LCD_SUPPORTED',
        'SOC_PARLIO_SUPPORT_SPI_LCD',
        'SOC_PARLIO_SUPPORT_I80_LCD',
    ]
    assert rules['devices']['display_lcd_rgb'].all_of == ['SOC_LCDCAM_RGB_LCD_SUPPORTED']
    assert rules['devices']['display_lcd_i80'].all_of == ['SOC_LCDCAM_I80_LCD_SUPPORTED']
    assert rules['devices']['display_lcd_spi'].all_of == ['SOC_GPSPI_SUPPORTED']
    assert rules['devices']['littlefs_sdmmc'].all_of == ['SOC_SDMMC_HOST_SUPPORTED']
    assert rules['devices']['littlefs_spi'].all_of == ['SOC_GPSPI_SUPPORTED']
    assert rules['devices']['lcd_touch_spi'].all_of == ['SOC_GPSPI_SUPPORTED']
    assert rules['peripherals']['spi'].all_of == ['SOC_GPSPI_SUPPORTED']
    assert rules['capabilities']['i2s.supports_pdm_rx_hp_filter'].all_of == [
        'SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER'
    ]
    assert spec.gpio_macros.valid_input_mask == 'SOC_GPIO_VALID_GPIO_MASK'
    assert spec.gpio_macros.valid_output_mask == 'SOC_GPIO_VALID_OUTPUT_GPIO_MASK'
    assert spec.gpio_macros.input_range_max == 'SOC_GPIO_IN_RANGE_MAX'
    assert spec.gpio_macros.output_range_max == 'SOC_GPIO_OUT_RANGE_MAX'
    assert spec.hardware_limits['i2s.hw_version'].sources[0].symbol == 'SOC_I2S_HW_VERSION_2'
    assert spec.hardware_limits['i2s.hw_version'].sources[0].value == 2
    assert not hasattr(spec, 'supported_since_overrides')
    assert spec.hardware_limits['i2c.hp_instance_count'].sources[0].symbol == 'SOC_HP_I2C_NUM'
    assert not spec.hardware_limits['i2c.hp_instance_count'].applies_to
    assert spec.hardware_limits['i2c.lp_instance_count'].sources[0].symbol == 'SOC_LP_I2C_NUM'
    assert not spec.hardware_limits['i2c.lp_instance_count'].applies_to
    assert spec.hardware_limits['i2s.instance_count'].sources[0].kind == 'soc_caps_macro'
    assert spec.hardware_limits['i2s.instance_count'].sources[0].symbol == 'SOC_I2S_NUM'
    assert not spec.hardware_limits['i2s.instance_count'].applies_to
    assert spec.hardware_limits['i2s.pdm_max_tx_lines'].sources[0].symbol == 'SOC_I2S_PDM_MAX_TX_LINES'
    assert spec.hardware_limits['i2s.pdm_max_rx_lines'].sources[0].symbol == 'SOC_I2S_PDM_MAX_RX_LINES'
    assert spec.hardware_limits['adc.unit_count'].sources[0].symbol == 'SOC_ADC_PERIPH_NUM'
    assert spec.hardware_limits['adc.unit_count'].applies_to[0].path == ['unit_id']
    assert spec.hardware_limits['adc.unit_count'].applies_to[0].check == 'value'
    assert spec.hardware_limits['adc.unit_count'].applies_to[1].path == ['patterns', 'unit']
    assert spec.hardware_limits['adc.unit_count'].applies_to[1].check == 'value'
    assert spec.hardware_limits['adc.max_channel_count'].sources[0].symbol == 'SOC_ADC_MAX_CHANNEL_NUM'
    assert spec.hardware_limits['adc.max_channel_count'].applies_to[0].compare == 'lt'
    assert spec.hardware_limits['adc.pattern_length_max'].applies_to[1].path == ['channel_list']
    assert spec.hardware_limits['anacmpr.unit_count'].sources[0].symbol == 'SOC_ANA_CMPR_NUM'
    assert spec.hardware_limits['anacmpr.unit_count'].applies_to[0].path == ['unit']
    assert spec.hardware_limits['anacmpr.unit_count'].applies_to[0].compare == 'lt'
    assert spec.hardware_limits['ledc.channel_count'].applies_to[0].path == ['channel']
    assert spec.hardware_limits['ledc.channel_count'].applies_to[0].compare == 'lt'
    assert spec.hardware_limits['sdmmc.slot_count'].applies_to[0].path == ['sub_config', 'slot']
    assert spec.hardware_limits['sdmmc.slot_count'].applies_to[0].compare == 'lt'
    assert spec.hardware_limits['sdmmc.slot_count'].applies_to[1].type == 'littlefs'
    assert spec.hardware_limits['pcnt.channels_per_unit'].applies_to[0].path == ['channel_count']
    assert spec.hardware_limits['pcnt.watch_points_per_unit'].sources[0].symbol == 'SOC_PCNT_THRES_POINT_PER_UNIT'
    assert spec.hardware_limits['mcpwm.group_count'].applies_to[0].path == ['timer_config', 'group_id']
    assert spec.hardware_limits['mcpwm.group_count'].applies_to[0].compare == 'lt'
    assert spec.hardware_limits['mcpwm.group_count'].applies_to[1].path == ['operator_config', 'group_id']
    assert spec.hardware_limits['mcpwm.comparators_per_operator'].applies_to[0].path == ['comparator_configs']
    assert spec.hardware_limits['mcpwm.comparators_per_operator'].applies_to[0].check == 'arrayLength'
    assert spec.hardware_limits['lcd.rgb_data_width'].sources[0].symbol == 'SOC_LCDCAM_RGB_DATA_WIDTH'


def test_board_creator_filters_allof_anyof_soc_rules(monkeypatch, tmp_path: Path) -> None:
    sys.path.insert(0, str(Path(__file__).parent.parent.parent))
    from create_new_board import BoardCreator

    idf = tmp_path / 'idf'
    _write_soc_caps(
        idf,
        'esp32p4',
        """
        #define SOC_PARLIO_SUPPORTED 1
        #define SOC_PARLIO_LCD_SUPPORTED 1
        """,
    )
    monkeypatch.setenv('IDF_PATH', str(idf))

    creator = BoardCreator(Path(__file__).parent.parent.parent)

    assert creator.filter_by_chip_capability('esp32p4', ['display_lcd_parlio'], 'devices') == [
        'display_lcd_parlio'
    ]

    _write_soc_caps(
        idf,
        'esp32c3',
        """
        #define SOC_PARLIO_SUPPORTED 1
        """,
    )

    assert creator.filter_by_chip_capability('esp32c3', ['display_lcd_parlio'], 'devices') == []
