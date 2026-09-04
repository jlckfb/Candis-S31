# SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

# I2S peripheral config parser
# Supports different I2S hardware versions (must match ESP-IDF i2s_std.h):
# - SOC_I2S_HW_VERSION_1: legacy IP (msb_right in std slot cfg; no ext_clk_freq_hz in std clk cfg)
# - SOC_I2S_HW_VERSION_2: newer IP (left_align, big_endian, bit_order_lsb; ext_clk_freq_hz in std clk)
# Layout is taken from the SoC capability catalog; parser-only fallback keeps
# esp32/esp32s2 on v1 and other chips on v2 when no catalog context is configured.
VERSION = 'v1.0.0'

PERIPH_I2S_IO_LIST = {
    'std': [
        'mclk',
        'bclk',
        'ws',
        'dout',
        'din',
    ],
    'tdm': [
        'mclk',
        'bclk',
        'ws',
        'dout',
        'din',
    ],
    'pdm': [
        'clk',
        'dout',
        'dout2',
        'din',
        'dins',
    ],
}

import sys

import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'generators'))
from generators.utils.soc_capability_query import current_soc

def get_includes() -> list:
    """Return list of required include headers for I2S peripheral"""
    return [
        'periph_i2s.h',
    ]

def validate_enum_value(value: str, enum_type: str) -> bool:
    """Validate if the enum value is within the valid range for ESP-IDF I2S driver.

    Args:
        value: The enum value to validate
        enum_type: The type of enum (clk_src, slot_mode, slot_mask, etc.)

    Returns:
        True if valid, False otherwise
    """
    # Define valid enum values based on ESP-IDF I2S driver
    valid_enums = {
        'clk_src': [
            'I2S_CLK_SRC_DEFAULT',
            'I2S_CLK_SRC_EXTERNAL',
            'I2S_CLK_SRC_APB',
            'I2S_CLK_SRC_PLL_F160M',
            'I2S_CLK_SRC_PLL_F240M'
        ],
        'slot_mode': [
            'I2S_SLOT_MODE_MONO',
            'I2S_SLOT_MODE_STEREO'
        ],
        'slot_bit_width': [
            'I2S_SLOT_BIT_WIDTH_AUTO',
            'I2S_SLOT_BIT_WIDTH_8BIT',
            'I2S_SLOT_BIT_WIDTH_16BIT',
            'I2S_SLOT_BIT_WIDTH_24BIT',
            'I2S_SLOT_BIT_WIDTH_32BIT'
        ],
        'std_slot_mask': [
            'I2S_STD_SLOT_LEFT',
            'I2S_STD_SLOT_RIGHT',
            'I2S_STD_SLOT_BOTH'
        ],
        'tdm_slot_mask': [
            'I2S_TDM_SLOT0',
            'I2S_TDM_SLOT1',
            'I2S_TDM_SLOT2',
            'I2S_TDM_SLOT3',
            'I2S_TDM_SLOT4',
            'I2S_TDM_SLOT5',
            'I2S_TDM_SLOT6',
            'I2S_TDM_SLOT7',
            'I2S_TDM_SLOT8',
            'I2S_TDM_SLOT9',
            'I2S_TDM_SLOT10',
            'I2S_TDM_SLOT11',
            'I2S_TDM_SLOT12',
            'I2S_TDM_SLOT13',
            'I2S_TDM_SLOT14',
            'I2S_TDM_SLOT15'
        ],
        'data_bit_width': [
            'I2S_DATA_BIT_WIDTH_8BIT',
            'I2S_DATA_BIT_WIDTH_16BIT',
            'I2S_DATA_BIT_WIDTH_24BIT',
            'I2S_DATA_BIT_WIDTH_32BIT'
        ],
        'mclk_multiple': [
            'I2S_MCLK_MULTIPLE_128',
            'I2S_MCLK_MULTIPLE_192',
            'I2S_MCLK_MULTIPLE_256',
            'I2S_MCLK_MULTIPLE_384',
            'I2S_MCLK_MULTIPLE_512',
            'I2S_MCLK_MULTIPLE_576',
            'I2S_MCLK_MULTIPLE_768',
            'I2S_MCLK_MULTIPLE_1024',
            'I2S_MCLK_MULTIPLE_1152'
        ],
        'pdm_data_fmt': [
            'I2S_PDM_DATA_FMT_PCM',
            'I2S_PDM_DATA_FMT_RAW'
        ],
        'pdm_sig_scale': [
            'I2S_PDM_SIG_SCALING_DIV_2',
            'I2S_PDM_SIG_SCALING_MUL_1',
            'I2S_PDM_SIG_SCALING_MUL_2',
            'I2S_PDM_SIG_SCALING_MUL_4'
        ],
        'pdm_line_mode': [
            'I2S_PDM_TX_ONE_LINE_CODEC',
            'I2S_PDM_TX_ONE_LINE_DAC',
            'I2S_PDM_TX_TWO_LINE_DAC'
        ],
        'pdm_tx_line_mode': [
            'I2S_PDM_TX_ONE_LINE_CODEC',
            'I2S_PDM_TX_ONE_LINE_DAC',
            'I2S_PDM_TX_TWO_LINE_DAC'
        ],
        'pdm_slot_mask': [
            'I2S_PDM_SLOT_LEFT',
            'I2S_PDM_SLOT_RIGHT',
            'I2S_PDM_SLOT_BOTH',
            'I2S_PDM_RX_LINE0_SLOT_LEFT',
            'I2S_PDM_RX_LINE0_SLOT_RIGHT',
            'I2S_PDM_RX_LINE1_SLOT_LEFT',
            'I2S_PDM_RX_LINE1_SLOT_RIGHT',
            'I2S_PDM_RX_LINE2_SLOT_LEFT',
            'I2S_PDM_RX_LINE2_SLOT_RIGHT',
            'I2S_PDM_RX_LINE3_SLOT_LEFT',
            'I2S_PDM_RX_LINE3_SLOT_RIGHT',
            'I2S_PDM_LINE_SLOT_ALL'
        ],
        'pdm_dsr': [
            'I2S_PDM_DSR_8S',
            'I2S_PDM_DSR_16S'
        ]
    }

    if enum_type not in valid_enums:
        print(f"⚠️  WARNING: Unknown enum type '{enum_type}' for validation")
        return True  # Allow unknown types to pass

    if value not in valid_enums[enum_type]:
        print(f"❌ YAML VALIDATION ERROR: Invalid {enum_type} value '{value}'!")
        print(f'      File: board_peripherals.yaml')
        print(f'      Peripheral: I2S configuration')
        print(f"   ❌ Invalid value: '{value}'")
        print(f'   ✅ Valid values: {valid_enums[enum_type]}')
        print(f'   ℹ️ Please use one of the valid enum values listed above')
        return False

    return True

def get_enum_value(value: str, full_enum: str, enum_type: str = None) -> str:
    """Helper function to get enum value with validation.

    Args:
        value: The value from YAML config
        full_enum: The complete ESP-IDF enum value to use if value is not provided
        enum_type: The type of enum for validation (optional)

    Returns:
        The enum value to use

    Raises:
        ValueError: If validation fails and the value is not empty
    """
    if not value:
        result = full_enum
    else:
        result = value

    # Validate the enum value if enum_type is provided
    if enum_type and not validate_enum_value(result, enum_type):
        # If validation fails and we have a non-empty value, raise an exception
        if value:  # Only raise if user provided a value (not empty)
            raise ValueError(f"Invalid {enum_type} value '{value}'. Please use one of the valid enum values.")
        else:
            # If no value provided, use default and show warning
            print(f"⚠️  WARNING: Using default value '{full_enum}' due to validation failure")
            result = full_enum

    return result

def parse_i2s_format(format_str: str) -> dict:
    """Parse I2S format string into configuration values.

    Args:
        format_str: Format string (e.g. 'std-out', 'pdm-out', 'std-in')

    Returns:
        Dictionary containing mode, direction and config_type values
    """
    if not format_str:
        return {
            'mode': 'I2S_COMM_MODE_STD',
            'direction': 'I2S_DIR_TX',  # Default to TX
            'config_type': 'std'
        }

    parts = format_str.lower().split('-')
    result = {}

    # Parse mode and config type
    if 'std' in parts:
        result['mode'] = 'I2S_COMM_MODE_STD'
        result['config_type'] = 'std'
    elif 'tdm' in parts:
        result['mode'] = 'I2S_COMM_MODE_TDM'
        result['config_type'] = 'tdm'
    elif 'pdm' in parts:
        result['mode'] = 'I2S_COMM_MODE_PDM'
        result['config_type'] = 'pdm'
    else:
        result['mode'] = 'I2S_COMM_MODE_STD'  # Default to standard mode
        result['config_type'] = 'std'

    # Parse direction
    if 'out' in parts:
        result['direction'] = 'I2S_DIR_TX'  # TX for output
    elif 'in' in parts:
        result['direction'] = 'I2S_DIR_RX'  # RX for input
    else:
        result['direction'] = 'I2S_DIR_TX'  # Default to TX

    return result

def get_effective_chip_name():
    """Resolve target chip for I2S parser compatibility fallback.

    Normal generation configures ``current_soc()`` from board metadata before
    parser dispatch. This fallback is only for direct parser tests or legacy
    calls that have not configured the catalog context.
    """
    from generators.utils.board_utils import get_chip_name
    chip_name = get_chip_name()
    if chip_name:
        return chip_name.strip().lower()
    for env_key in ('IDF_TARGET',):
        raw = os.environ.get(env_key, '').strip().lower().replace('-', '')
        if raw:
            return raw
    return None


def chip_supports_pdm_tx_second_dout_pin(chip_name) -> bool:
    """Whether ``i2s_pdm_tx_gpio_config_t`` includes ``dout2`` (``SOC_I2S_PDM_MAX_TX_LINES > 1``)."""
    max_lines = current_soc().limit('i2s.pdm_max_tx_lines', default=1)
    return _int_or_default(max_lines, 1) > 1


def chip_supports_pdm_rx_hp_filter(chip_name) -> bool:
    """Whether ``i2s_pdm_rx_slot_config_t`` exposes HP filter fields."""
    return current_soc().supports('i2s.supports_pdm_rx_hp_filter', default=False)


def get_i2s_hw_version() -> int:
    """Determine I2S hardware version for generated struct layout.

    The shared SoC catalog is authoritative. When no catalog context is
    configured, keep a small parser-only fallback for direct parser tests.

    Returns:
        1 for SOC_I2S_HW_VERSION_1 layout, 2 for SOC_I2S_HW_VERSION_2 layout.
    """
    soc_hw_version = current_soc().limit('i2s.hw_version', default=None)
    if soc_hw_version in (1, 2):
        return int(soc_hw_version)

    chip_name = get_effective_chip_name()
    if not chip_name:
        return 2
    if chip_name in ('esp32', 'esp32s2'):
        return 1
    return 2


def pdm_slot_supports_data_fmt() -> bool:
    """Whether current ESP-IDF exposes ``data_fmt`` in PDM RX/TX slot config.

    The field is present starting from IDF 5.5.x. When version cannot be resolved,
    prefer omitting the field because zero-initialization keeps PCM default behavior
    on newer IDF while avoiding compile failures on older IDF.
    """
    from generators.utils.idf_version import get_idf_version

    return get_idf_version() >= (5, 5, 0)


def std_clk_supports_bclk_div() -> bool:
    """Whether current ESP-IDF exposes ``bclk_div`` in ``i2s_std_clk_config_t``.

    The field is present starting from IDF 5.5.x. When version cannot be resolved,
    prefer omitting the field so generated config remains compatible with older IDF.
    """
    from generators.utils.idf_version import get_idf_version

    return get_idf_version() >= (5, 5, 0)


def default_tdm_slot_mask(slot_mode) -> str:
    """Return parser default TDM active slots for an ESP-IDF slot mode value."""
    if slot_mode == 'I2S_SLOT_MODE_MONO':
        return 'I2S_TDM_SLOT0'
    return 'I2S_TDM_SLOT0 | I2S_TDM_SLOT1'


def _int_or_default(value, default: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return int(default)


def _clock_gpio_direction(role: str) -> str:
    if role == 'I2S_ROLE_MASTER':
        return 'output'
    if role == 'I2S_ROLE_SLAVE':
        return 'input'
    return 'any'


def _gpio_value(pins: dict, field: str) -> int:
    return int(pins.get(field, -1))


def _validate_i2s_gpio_pin(name: str, field: str, pin: int, direction: str) -> None:
    if not current_soc().valid_gpio(pin, direction=direction, allow_nc=True, default=True):
        raise ValueError(
            f"Invalid I2S GPIO configuration for peripheral '{name}': "
            f"pin '{field}' ({pin}) is not a valid {direction} GPIO for the current SoC"
        )


def _validate_i2s_gpio_pins(name: str, config_type: str, direction: str, role: str, pins: dict) -> None:
    """Validate configured I2S GPIOs against the current SoC catalog.

    Unknown catalogs fail open through ``current_soc().valid_gpio(..., default=True)``.
    """
    clock_dir = _clock_gpio_direction(role)
    if config_type in ('std', 'tdm'):
        for field in ('mclk', 'bclk', 'ws'):
            _validate_i2s_gpio_pin(name, field, _gpio_value(pins, field), clock_dir)
        _validate_i2s_gpio_pin(name, 'dout', _gpio_value(pins, 'dout'), 'output')
        _validate_i2s_gpio_pin(name, 'din', _gpio_value(pins, 'din'), 'input')
        return

    if config_type == 'pdm':
        _validate_i2s_gpio_pin(name, 'clk', _gpio_value(pins, 'clk'), clock_dir)
        if direction == 'I2S_DIR_TX':
            _validate_i2s_gpio_pin(name, 'dout', _gpio_value(pins, 'dout'), 'output')
            if 'dout2' in pins:
                _validate_i2s_gpio_pin(name, 'dout2', _gpio_value(pins, 'dout2'), 'output')
        else:
            if any(f'din{i}' in pins for i in range(4)):
                for i in range(_pdm_rx_max_lines(pins)):
                    _validate_i2s_gpio_pin(name, f'din{i}', _gpio_value(pins, f'din{i}'), 'input')
            else:
                _validate_i2s_gpio_pin(name, 'din', _gpio_value(pins, 'din'), 'input')


def _pdm_rx_max_lines(pins: dict) -> int:
    max_lines = current_soc().limit('i2s.pdm_max_rx_lines', default=1)
    max_lines = _int_or_default(max_lines, 1)
    return max(1, min(4, max_lines))


def _validate_pdm_rx_line_limit(name: str, pins: dict, max_lines: int) -> None:
    for i in range(max_lines, 4):
        if f'din{i}' in pins:
            raise ValueError(
                f"Invalid I2S GPIO configuration for peripheral '{name}': "
                f"pin 'din{i}' is not supported because the current SoC supports "
                f'{max_lines} PDM RX data line(s)'
            )


def is_duplicate_io_conflict(duplicate_group: list) -> bool:
    """Return whether a pairwise I2S duplicate should warn as an IO conflict."""
    if len(duplicate_group) != 2:
        return True

    def normalize_port(port):
        if isinstance(port, str) and port.startswith('I2S_NUM_'):
            try:
                return int(port.removeprefix('I2S_NUM_'))
            except ValueError:
                return port
        return port

    shared_clock_fields = {'mclk', 'bclk', 'ws'}
    shared_data_fields = {'dout', 'din', 'dout2', 'dins'}
    shared_fields_by_config = {
        'std': shared_clock_fields | shared_data_fields,
        'tdm': shared_clock_fields | shared_data_fields,
        'pdm': {'clk'},
    }

    first_field = str(duplicate_group[0].get('io_field', '')).strip().lower()
    second_field = str(duplicate_group[1].get('io_field', '')).strip().lower()
    if first_field != second_field:
        return True

    ports = set()
    directions = set()
    config_types = set()

    for record in duplicate_group:
        artifact = record.get('artifact', {}) if isinstance(record, dict) else {}
        raw = artifact.get('raw', {}) if isinstance(artifact, dict) else {}
        raw_config = raw.get('config', {}) if isinstance(raw, dict) else {}
        result = artifact.get('result', {}) if isinstance(artifact, dict) else {}
        struct_init = result.get('struct_init', {}) if isinstance(result, dict) else {}

        port = raw_config.get('port')
        if port is None:
            port = normalize_port(struct_init.get('port'))
        format_name = artifact.get('format')
        if port is None or not format_name:
            return True

        parsed_format = parse_i2s_format(format_name)
        config_type = parsed_format.get('config_type')
        if config_type not in shared_fields_by_config:
            return True

        ports.add(normalize_port(port))
        directions.add(struct_init.get('direction') or parsed_format.get('direction'))
        config_types.add(config_type)

    if len(ports) != 1 or len(config_types) != 1:
        return True

    if directions != {'I2S_DIR_TX', 'I2S_DIR_RX'}:
        return True

    config_type = next(iter(config_types))
    if first_field not in shared_fields_by_config[config_type]:
        return True

    return False


def validate_pdm_data_fmt_compat(cfg: dict) -> None:
    """Reject explicit RAW/advanced PDM data format on older IDF branches."""
    if pdm_slot_supports_data_fmt():
        return

    data_fmt = cfg.get('data_fmt')
    if data_fmt and data_fmt != 'I2S_PDM_DATA_FMT_PCM':
        raise ValueError(
            f"PDM data_fmt '{data_fmt}' requires ESP-IDF >= 5.5. "
            'Use I2S_PDM_DATA_FMT_PCM or upgrade the IDF version.'
        )

def build_std_slot_config(cfg: dict, hw_version: int) -> dict:
    """Build slot configuration based on hardware version.
    Args:
        cfg: Configuration dictionary from YAML
        hw_version: Hardware version (1 or 2)
    Returns:
        Dictionary containing slot configuration
    """
    slot_cfg = {
        'data_bit_width': f"I2S_DATA_BIT_WIDTH_{cfg.get('data_bit_width', 16)}BIT",
        'slot_bit_width': get_enum_value(cfg.get('slot_bit_width'), 'I2S_SLOT_BIT_WIDTH_AUTO', 'slot_bit_width'),
        'slot_mode': get_enum_value(cfg.get('slot_mode'), 'I2S_SLOT_MODE_STEREO', 'slot_mode'),
        'slot_mask': get_enum_value(cfg.get('slot_mask'), 'I2S_STD_SLOT_BOTH', 'std_slot_mask'),
        'ws_width': int(cfg.get('ws_width', 16)),
        'ws_pol': bool(cfg.get('ws_pol', False)),
        'bit_shift': bool(cfg.get('bit_shift', True))
    }

    # Add hardware version specific fields
    if hw_version == 1:  # SOC_I2S_HW_VERSION_1 (ESP32/ESP32-S2)
        slot_cfg['msb_right'] = bool(cfg.get('msb_right', True))
    else:  # SOC_I2S_HW_VERSION_2 (all other chips)
        slot_cfg['left_align'] = bool(cfg.get('left_align', True))
        slot_cfg['big_endian'] = bool(cfg.get('big_endian', False))
        slot_cfg['bit_order_lsb'] = bool(cfg.get('bit_order_lsb', False))

    return slot_cfg

def build_pdm_tx_slot_config(cfg: dict, hw_version: int) -> dict:
    """Build PDM TX slot configuration based on hardware version.
    Args:
        cfg: Configuration dictionary from YAML
        hw_version: Hardware version (1 or 2)
    Returns:
        Dictionary containing PDM TX slot configuration
    """
    validate_pdm_data_fmt_compat(cfg)

    slot_cfg = {
        'data_bit_width': f"I2S_DATA_BIT_WIDTH_{cfg.get('data_bit_width', 16)}BIT",
        'slot_bit_width': get_enum_value(cfg.get('slot_bit_width'), 'I2S_SLOT_BIT_WIDTH_AUTO', 'slot_bit_width'),
        'slot_mode': get_enum_value(cfg.get('slot_mode'), 'I2S_SLOT_MODE_STEREO', 'slot_mode'),
        'sd_prescale': int(cfg.get('sd_prescale', 0)),
        'sd_scale': get_enum_value(cfg.get('sd_scale'), 'I2S_PDM_SIG_SCALING_MUL_1', 'pdm_sig_scale'),
        'hp_scale': get_enum_value(cfg.get('hp_scale'), 'I2S_PDM_SIG_SCALING_MUL_1', 'pdm_sig_scale'),
        'lp_scale': get_enum_value(cfg.get('lp_scale'), 'I2S_PDM_SIG_SCALING_MUL_1', 'pdm_sig_scale'),
        'sinc_scale': get_enum_value(cfg.get('sinc_scale'), 'I2S_PDM_SIG_SCALING_MUL_1', 'pdm_sig_scale')
    }

    if pdm_slot_supports_data_fmt():
        slot_cfg['data_fmt'] = get_enum_value(cfg.get('data_fmt'), 'I2S_PDM_DATA_FMT_PCM', 'pdm_data_fmt')

    # Add hardware version specific fields
    if hw_version == 1:  # SOC_I2S_HW_VERSION_1 (ESP32/ESP32-S2)
        slot_cfg['slot_mask'] = get_enum_value(cfg.get('slot_mask'), 'I2S_PDM_SLOT_BOTH', 'pdm_slot_mask')
    else:  # SOC_I2S_HW_VERSION_2 (all other chips)
        slot_cfg['line_mode'] = get_enum_value(cfg.get('line_mode'), 'I2S_PDM_TX_ONE_LINE_CODEC', 'pdm_tx_line_mode')
        slot_cfg['hp_en'] = bool(cfg.get('hp_en', True))
        # Validate HP filter cut-off frequency (23.3Hz ~ 185Hz)
        hp_freq = float(cfg.get('hp_cut_off_freq_hz', 35.5))
        if hp_freq < 23.3 or hp_freq > 185.0:
            print(f'⚠️  WARNING: HP filter cut-off frequency {hp_freq}Hz is out of range (2 3.3Hz ~ 185Hz), clamping to valid range')
            hp_freq = max(23.3, min(185.0, hp_freq))
        slot_cfg['hp_cut_off_freq_hz'] = hp_freq
        slot_cfg['sd_dither'] = int(cfg.get('sd_dither', 0))
        slot_cfg['sd_dither2'] = int(cfg.get('sd_dither2', 1))

    return slot_cfg

def build_pdm_rx_slot_config(cfg: dict, hw_version: int) -> dict:
    """Build PDM RX slot configuration based on hardware version.

    Args:
        cfg: Configuration dictionary from YAML
        hw_version: Hardware version (1 or 2)

    Returns:
        Dictionary containing PDM RX slot configuration
    """
    validate_pdm_data_fmt_compat(cfg)

    slot_cfg = {
        'data_bit_width': f"I2S_DATA_BIT_WIDTH_{cfg.get('data_bit_width', 16)}BIT",
        'slot_bit_width': get_enum_value(cfg.get('slot_bit_width'), 'I2S_SLOT_BIT_WIDTH_AUTO', 'slot_bit_width'),
        'slot_mode': get_enum_value(cfg.get('slot_mode'), 'I2S_SLOT_MODE_STEREO', 'slot_mode'),
        'slot_mask': get_enum_value(cfg.get('slot_mask'), 'I2S_PDM_SLOT_BOTH', 'pdm_slot_mask')
    }

    if pdm_slot_supports_data_fmt():
        slot_cfg['data_fmt'] = get_enum_value(cfg.get('data_fmt'), 'I2S_PDM_DATA_FMT_PCM', 'pdm_data_fmt')

    # These fields are gated by SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER, not by
    # SOC_I2S_HW_VERSION_2. ESP32-S3 is HW_VERSION_2 but lacks the cap.
    if hw_version == 2 and chip_supports_pdm_rx_hp_filter(get_effective_chip_name()):
        slot_cfg['hp_en'] = bool(cfg.get('hp_en', True))
        # Validate HP filter cut-off frequency (23.3Hz ~ 185Hz)
        hp_freq = float(cfg.get('hp_cut_off_freq_hz', 35.5))
        if hp_freq < 23.3 or hp_freq > 185.0:
            print(f'⚠️  WARNING: HP filter cut-off frequency {hp_freq}Hz is out of range (23.3Hz ~ 185Hz), clamping to valid range')
            hp_freq = max(23.3, min(185.0, hp_freq))
        slot_cfg['hp_cut_off_freq_hz'] = hp_freq
        slot_cfg['amplify_num'] = int(cfg.get('amplify_num', 1))

    return slot_cfg

def parse(name: str, config: dict) -> dict:
    """Parse I2S peripheral configuration from YAML to C structure
    Args:
        name: Peripheral name
        config: Configuration dictionary from YAML
    Returns:
        Dictionary containing C structure configuration
    Raises:
        ValueError: If YAML validation fails
    """
    try:
        # Get format string: check both top-level and config-level
        format_str = config.get('format', '')  # Get format from parent scope
        if not format_str:
            # Also check inside config dict (for YAML where format is under config)
            cfg_temp = config.get('config', {})
            format_str = cfg_temp.get('format', '') if isinstance(cfg_temp, dict) else ''
        if not format_str:
            print(f'WARNING: No format string provided for {name}, using default std-out')
            format_str = 'std-out'
        format_config = parse_i2s_format(format_str)
        # Use logger for debug output
        from generators.utils.logger import get_logger
        logger = get_logger('periph_i2s')
        logger.debug(f"I2S {name} format '{format_str}' to: {format_config}")
        # Get role from parent scope
        role = config.get('role', 'master')
        role = f'I2S_ROLE_{role.upper()}'

        # Get the config dictionary
        cfg = config.get('config', {})

        # Get direction: priority is YAML config > format string parsing
        # i2s_dir_t is a bit flag type: I2S_DIR_RX = BIT(0), I2S_DIR_TX = BIT(1)
        # It can be a single enum value or a bitwise combination (e.g., "I2S_DIR_RX | I2S_DIR_TX")
        direction = cfg.get('direction')
        if direction:
            # Validate direction if directly specified in YAML
            # Allow single enum values or C constant expressions (e.g., "I2S_DIR_RX | I2S_DIR_TX")
            valid_single_directions = ['I2S_DIR_RX', 'I2S_DIR_TX']
            if direction in valid_single_directions:
                # Valid single direction value
                pass
            elif 'I2S_DIR_' in direction:
                # Looks like a C constant expression, allow it (will be validated by compiler)
                pass
            else:
                raise ValueError(f'Invalid direction: {direction}. Valid values: {valid_single_directions} or C constant expressions')
        else:
            # Use direction from format string parsing
            direction = format_config['direction']

        # Get port from config, with fallback to parsing from name for backward compatibility
        port = cfg.get('port', 0)  # Default to port 0 if not specified

        # Get pins and invert_flags configurations
        pins = cfg.get('pins', {})
        invert_flags = cfg.get('invert_flags', {})
        _validate_i2s_gpio_pins(name, format_config['config_type'], direction, role, pins)

        # Determine I2S hardware version
        eff_chip = get_effective_chip_name()
        hw_version = get_i2s_hw_version()
        logger.debug(
            f'I2S {name} chip={eff_chip!r} hw_version={hw_version} '
            f"({'SOC_I2S_HW_VERSION_1' if hw_version == 1 else 'SOC_I2S_HW_VERSION_2'})"
        )

        # Create base config
        config_dict = {
            'struct_type': 'periph_i2s_config_t',
            'struct_var': f'{name}_cfg',
            'struct_init': {
                'port': f'I2S_NUM_{port}',
                'role': role,
                'mode': format_config['mode'],
                'direction': direction,
                'i2s_cfg': {
                    format_config['config_type']: {}  # This will be filled below
                }
            }
        }

        # Add mode-specific config
        if format_config['config_type'] == 'std':
            # Clock config — ``ext_clk_freq_hz`` depends on HW layout; ``bclk_div`` depends on IDF version.
            sr = int(cfg.get('sample_rate_hz', 48000))
            clk_src = get_enum_value(cfg.get('clk_src'), 'I2S_CLK_SRC_DEFAULT', 'clk_src')
            mclk_mul = 'I2S_MCLK_MULTIPLE_' + str(cfg.get('mclk_multiple', 256))
            clk_cfg = {
                'sample_rate_hz': sr,
                'clk_src': clk_src,
            }
            if hw_version == 2:
                clk_cfg['ext_clk_freq_hz'] = int(cfg.get('ext_clk_freq_hz', 0))
            clk_cfg['mclk_multiple'] = mclk_mul
            if std_clk_supports_bclk_div():
                clk_cfg['bclk_div'] = int(cfg.get('bclk_div', 8))

            config_dict['struct_init']['i2s_cfg']['std'] = {
                'clk_cfg': clk_cfg,
                # Slot config - handle hardware version differences
                'slot_cfg': build_std_slot_config(cfg, hw_version),
                # GPIO config
                'gpio_cfg': {
                    'mclk': int(pins.get('mclk', -1)),
                    'bclk': int(pins.get('bclk', -1)),
                    'ws': int(pins.get('ws', -1)),
                    'dout': int(pins.get('dout', -1)),
                    'din': int(pins.get('din', -1)),
                    'invert_flags': {
                        'mclk_inv': bool(invert_flags.get('mclk_inv', False)),
                        'bclk_inv': bool(invert_flags.get('bclk_inv', False)),
                        'ws_inv': bool(invert_flags.get('ws_inv', False))
                    }
                }
            }

        elif format_config['config_type'] == 'tdm':
            # Clock config
            config_dict['struct_init']['i2s_cfg']['tdm'] = {
                'clk_cfg': {
                    'sample_rate_hz': int(cfg.get('sample_rate_hz', 48000)),
                    'clk_src': get_enum_value(cfg.get('clk_src'), 'I2S_CLK_SRC_DEFAULT', 'clk_src'),
                    'ext_clk_freq_hz': int(cfg.get('ext_clk_freq_hz', 0)),  # Only used when clk_src is EXTERNAL
                    'mclk_multiple': 'I2S_MCLK_MULTIPLE_' + str(cfg.get('mclk_multiple', 256)),
                    'bclk_div': int(cfg.get('bclk_div', 8))
                },
                # Slot config
                'slot_cfg': {
                    'data_bit_width': f"I2S_DATA_BIT_WIDTH_{cfg.get('data_bit_width', 16)}BIT",
                    'slot_bit_width': get_enum_value(cfg.get('slot_bit_width'), 'I2S_SLOT_BIT_WIDTH_AUTO', 'slot_bit_width'),
                    'slot_mode': get_enum_value(cfg.get('slot_mode'), 'I2S_SLOT_MODE_STEREO', 'slot_mode'),
                    'slot_mask': cfg.get('slot_mask', default_tdm_slot_mask(cfg.get('slot_mode', 'I2S_SLOT_MODE_STEREO'))),
                    'ws_width': int(cfg.get('ws_width', 0)),  # I2S_TDM_AUTO_WS_WIDTH
                    'ws_pol': bool(cfg.get('ws_pol', False)),
                    'bit_shift': bool(cfg.get('bit_shift', True)),
                    'left_align': bool(cfg.get('left_align', False)),
                    'big_endian': bool(cfg.get('big_endian', False)),
                    'bit_order_lsb': bool(cfg.get('bit_order_lsb', False)),
                    'skip_mask': bool(cfg.get('skip_mask', False)),
                    'total_slot': int(cfg.get('total_slot', 0))  # I2S_TDM_AUTO_SLOT_NUM
                },
                # GPIO config
                'gpio_cfg': {
                    'mclk': int(pins.get('mclk', -1)),
                    'bclk': int(pins.get('bclk', -1)),
                    'ws': int(pins.get('ws', -1)),
                    'dout': int(pins.get('dout', -1)),
                    'din': int(pins.get('din', -1)),
                    'invert_flags': {
                        'mclk_inv': bool(invert_flags.get('mclk_inv', False)),
                        'bclk_inv': bool(invert_flags.get('bclk_inv', False)),
                        'ws_inv': bool(invert_flags.get('ws_inv', False))
                    }
                }
            }

        elif format_config['config_type'] == 'pdm':
            if direction == 'I2S_DIR_TX':  # TX for output
                # PDM TX clock config - handle hardware version differences
                pdm_tx_clk_cfg = {
                    'sample_rate_hz': int(cfg.get('sample_rate_hz', 48000)),
                    'clk_src': get_enum_value(cfg.get('clk_src'), 'I2S_CLK_SRC_DEFAULT', 'clk_src'),
                    'mclk_multiple': 'I2S_MCLK_MULTIPLE_' + str(cfg.get('mclk_multiple', 256)),
                    'up_sample_fp': int(cfg.get('up_sample_fp', 960)),
                    'up_sample_fs': int(cfg.get('up_sample_fs', 480)),
                    'bclk_div': int(cfg.get('bclk_div', 8))
                }
                pdm_gpio_cfg = {
                    'clk': int(pins.get('clk', -1)),
                    'dout': int(pins.get('dout', -1)),
                    'invert_flags': {
                        'clk_inv': bool(invert_flags.get('clk_inv', False))
                    }
                }
                # dout2 exists only when SOC_I2S_PDM_MAX_TX_LINES > 1 (e.g. not on ESP32)
                if chip_supports_pdm_tx_second_dout_pin(eff_chip):
                    pdm_gpio_cfg['dout2'] = int(pins.get('dout2', -1)) if 'dout2' in pins else -1

                config_dict['struct_init']['i2s_cfg'] = {
                    'pdm_tx': {  # Use pdm_tx for output
                        'clk_cfg': pdm_tx_clk_cfg,
                        # Slot config
                        'slot_cfg': build_pdm_tx_slot_config(cfg, hw_version),
                        'gpio_cfg': pdm_gpio_cfg,
                    }
                }
            else:  # I2S_DIR_RX - Use PDM RX configuration for input direction
                # PDM RX clock config - handle hardware version differences
                pdm_rx_clk_cfg = {
                    'sample_rate_hz': int(cfg.get('sample_rate_hz', 48000)),
                    'clk_src': get_enum_value(cfg.get('clk_src'), 'I2S_CLK_SRC_DEFAULT', 'clk_src'),
                    'mclk_multiple': 'I2S_MCLK_MULTIPLE_' + str(cfg.get('mclk_multiple', 256)),
                    'dn_sample_mode': get_enum_value(cfg.get('dn_sample_mode'), 'I2S_PDM_DSR_8S', 'pdm_dsr'),
                    'bclk_div': int(cfg.get('bclk_div', 8))
                }
                pdm_rx_gpio_cfg = {
                    'clk': int(pins.get('clk', -1)),
                    'invert_flags': {
                        'clk_inv': bool(invert_flags.get('clk_inv', False))
                    },
                }
                max_rx_lines = _pdm_rx_max_lines(pins)
                if any(f'din{i}' in pins for i in range(4)):
                    _validate_pdm_rx_line_limit(name, pins, max_rx_lines)
                    pdm_rx_gpio_cfg['dins'] = [
                        int(pins.get(f'din{i}', -1))
                        for i in range(max_rx_lines)
                    ]
                else:
                    pdm_rx_gpio_cfg['din'] = int(pins.get('din', -1)) if 'din' in pins else -1

                config_dict['struct_init']['i2s_cfg'] = {
                    'pdm_rx': {  # Use pdm_rx for input
                        'clk_cfg': pdm_rx_clk_cfg,
                        'slot_cfg': build_pdm_rx_slot_config(cfg, hw_version),
                        'gpio_cfg': pdm_rx_gpio_cfg,
                    }
                }

        return config_dict

    except ValueError as e:
        # Re-raise ValueError with more context
        raise ValueError(f"YAML validation error in I2S peripheral '{name}': {e}")
    except Exception as e:
        # Catch any other exceptions and provide context
        raise ValueError(f"Error parsing I2S peripheral '{name}': {e}")
