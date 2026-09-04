# SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

# I2C peripheral config parser
VERSION = 'v1.0.0'

PERIPH_I2C_IO_LIST = [
    'sda_io_num',
    'scl_io_num',
]

import sys
import re
from typing import Optional, Tuple, Union

from generators.utils.soc_capability_query import current_soc

I2CPortValue = Union[int, str]

VALID_I2C_PORT_MACROS = {
    'I2C_NUM_0',
    'I2C_NUM_1',
    'LP_I2C_NUM_0',
}

VALID_I2C_CLK_SOURCE_MACROS = {
    'I2C_CLK_SRC_DEFAULT',
    'I2C_CLK_SRC_APB',
    'I2C_CLK_SRC_REF_TICK',
    'I2C_CLK_SRC_XTAL',
    'I2C_CLK_SRC_RC_FAST',
    'LP_I2C_SCLK_DEFAULT',
    'LP_I2C_SCLK_LP_FAST',
    'LP_I2C_SCLK_XTAL_D2',
}
LP_I2C_CLK_SOURCE_MACROS = {
    'LP_I2C_SCLK_DEFAULT',
    'LP_I2C_SCLK_LP_FAST',
    'LP_I2C_SCLK_XTAL_D2',
}

def get_includes() -> list:
    """Return list of required include headers for I2C peripheral"""
    return [
        'driver/i2c_master.h',
        'driver/i2c_types.h',
        'hal/gpio_types.h'
    ]

def _validate_macro_value(name: str, field: str, value: str, valid_values: set) -> str:
    """Validate macro value against allowed macro list."""
    if value not in valid_values:
        raise ValueError(
            f"Invalid {field} value '{value}' in I2C peripheral '{name}'. "
            f'Valid values: {sorted(valid_values)}'
        )
    return value


def _soc_i2c_port_limits() -> Tuple[Optional[int], Optional[int], Optional[int]]:
    """Return total, HP, and LP I2C port counts from the active SoC catalog."""
    soc = current_soc()
    total = soc.limit('i2c.instance_count', default=None)
    hp = soc.limit('i2c.hp_instance_count', default=None)
    lp = soc.limit('i2c.lp_instance_count', default=None)
    if total is not None and hp is None:
        hp = total - (lp or 0)
    if total is None and hp is not None and lp is not None:
        total = hp + lp
    return total, hp, lp


def _validate_i2c_port_macro(name: str, value: str) -> str:
    total, hp, lp = _soc_i2c_port_limits()
    hp_match = re.fullmatch(r'I2C_NUM_(\d+)', value)
    if hp_match:
        index = int(hp_match.group(1))
        if hp is not None:
            if index < hp:
                return value
            raise ValueError(
                f"Invalid port value '{value}' in I2C peripheral '{name}'. "
                f'Valid regular I2C ports are I2C_NUM_0 ~ I2C_NUM_{max(hp - 1, 0)}.'
            )
        return _validate_macro_value(name, 'port', value, VALID_I2C_PORT_MACROS)

    lp_match = re.fullmatch(r'LP_I2C_NUM_(\d+)', value)
    if lp_match:
        index = int(lp_match.group(1))
        if lp is not None:
            if index < lp:
                return value
            raise ValueError(
                f"LP I2C port '{value}' in I2C peripheral '{name}' is not supported by the current SoC. "
                f'Current SoC supports {lp} LP I2C port(s).'
            )
        return _validate_macro_value(name, 'port', value, VALID_I2C_PORT_MACROS)

    raise ValueError(
        f"Invalid port value '{value}' in I2C peripheral '{name}'. "
        'Use -1, a numeric IDF i2c_port_t value, I2C_NUM_N, or LP_I2C_NUM_N.'
    )


def _parse_i2c_port(name: str, port_cfg) -> I2CPortValue:
    """Parse and validate I2C port value.

    Supports:
      - -1 for auto selection
      - numeric port index (e.g. 0/1 -> I2C_NUM_0/I2C_NUM_1)
      - explicit macro value (e.g. I2C_NUM_0 / LP_I2C_NUM_0)
    """
    if port_cfg is None or port_cfg == '':
        return -1

    if isinstance(port_cfg, str):
        port_cfg = port_cfg.strip()
        if not port_cfg:
            return -1
        if port_cfg.lstrip('-').isdigit():
            port_cfg = int(port_cfg)
        else:
            return _validate_i2c_port_macro(name, port_cfg)

    if isinstance(port_cfg, bool) or not isinstance(port_cfg, int):
        raise ValueError(f"Invalid port type '{type(port_cfg).__name__}' in I2C peripheral '{name}'")

    if port_cfg == -1:
        return -1

    if port_cfg < -1:
        raise ValueError(f"Invalid port value '{port_cfg}' in I2C peripheral '{name}'. Use -1, 0, 1, or valid macro.")

    total, hp, _lp = _soc_i2c_port_limits()
    if hp is not None:
        if port_cfg < hp:
            return f'I2C_NUM_{port_cfg}'
        if total is not None and port_cfg < total:
            return f'LP_I2C_NUM_{port_cfg - hp}'
        if total is not None:
            raise ValueError(
                f"Invalid port value '{port_cfg}' in I2C peripheral '{name}'. "
                f'Current SoC supports i2c_port_t values 0 ~ {max(total - 1, 0)}.'
            )

    return _validate_i2c_port_macro(name, f'I2C_NUM_{port_cfg}')


def _parse_i2c_clk_source(name: str, clk_cfg, i2c_port: I2CPortValue) -> str:
    """Parse and validate I2C clock source macro."""
    if clk_cfg is None or clk_cfg == '':
        if isinstance(i2c_port, str) and i2c_port.startswith('LP_I2C_NUM_'):
            return 'LP_I2C_SCLK_DEFAULT'
        return 'I2C_CLK_SRC_DEFAULT'

    if not isinstance(clk_cfg, str):
        raise ValueError(f"Invalid clk_source type '{type(clk_cfg).__name__}' in I2C peripheral '{name}'")

    return _validate_macro_value(name, 'clk_source', clk_cfg.strip(), VALID_I2C_CLK_SOURCE_MACROS)


def _validate_i2c_port_clk_source_combo(name: str, i2c_port: I2CPortValue, clk_source: str) -> None:
    """Validate that I2C port selection matches the chosen clock source family."""
    is_lp_port = isinstance(i2c_port, str) and i2c_port.startswith('LP_I2C_NUM_')
    is_lp_clk = clk_source in LP_I2C_CLK_SOURCE_MACROS

    if i2c_port == -1 and is_lp_clk:
        raise ValueError(
            f"LP I2C clock source '{clk_source}' in I2C peripheral '{name}' requires an explicit LP_I2C_NUM_* port"
        )
    if is_lp_port and not is_lp_clk:
        raise ValueError(
            f"Clock source '{clk_source}' is incompatible with LP I2C port '{i2c_port}' in I2C peripheral '{name}'"
        )
    if not is_lp_port and is_lp_clk:
        raise ValueError(
            f"Clock source '{clk_source}' is incompatible with regular I2C port '{i2c_port}' in I2C peripheral '{name}'"
        )

def _validate_i2c_soc_capabilities(name: str, i2c_port: I2CPortValue) -> None:
    """Validate I2C SoC capabilities while keeping unknown catalogs fail-open."""
    soc = current_soc()
    if isinstance(i2c_port, str) and i2c_port.startswith('LP_I2C_NUM_'):
        lp_supported = soc.supports('i2c.lp_supported', default=True)
        lp_instance_count = soc.limit('i2c.lp_instance_count', default=None)
        if not lp_supported or lp_instance_count == 0:
            raise ValueError(
                f"LP I2C port '{i2c_port}' in I2C peripheral '{name}' is not supported by the current SoC"
            )


def parse(name: str, config: dict) -> dict:
    """Parse I2C peripheral configuration.

    Args:
        name: Peripheral name (e.g. 'i2c-0')
        config: Configuration dictionary containing:
            - port: I2C port number
            - pins: SDA and SCL pin numbers
            - enable_internal_pullup: Enable internal pullup resistors
            - glitch_count: Glitch filter count
            - intr_priority: Interrupt priority

    Returns:
        Dictionary containing I2C configuration structure
    """
    try:
        # Get the actual config
        config = config.get('config', {})

        # Get port number (-1 for auto selection; no I2C_NUM_AUTO macro in ESP-IDF)
        i2c_port = _parse_i2c_port(name, config.get('port', -1))

        # Get pins from the config
        pins = config.get('pins', {})
        sda_io = int(pins.get('sda', -1))  # Convert to int, -1 means not set
        scl_io = int(pins.get('scl', -1))  # Convert to int, -1 means not set



        # Get pullup setting
        enable_internal_pullup = config.get('enable_internal_pullup', True)

        # Get glitch filter setting
        glitch_ignore_cnt = config.get('glitch_count', 7)  # Default to 7 if not set

        # Get interrupt priority setting
        intr_priority = config.get('intr_priority', 1)  # Default to 1 if not set
        if intr_priority is None or intr_priority == '':
            intr_priority = 1  # Set default if empty or None
        clk_source = _parse_i2c_clk_source(name, config.get('clk_source'), i2c_port)
        _validate_i2c_port_clk_source_combo(name, i2c_port, clk_source)
        _validate_i2c_soc_capabilities(name, i2c_port)

        struct_init = {
            'i2c_port': i2c_port,
            'sda_io_num': sda_io,
            'scl_io_num': scl_io,
            'glitch_ignore_cnt': glitch_ignore_cnt,
            'intr_priority': intr_priority,
            'trans_queue_depth': 0,  # Default queue depth
            'flags': {
                'enable_internal_pullup': enable_internal_pullup,
            },
        }
        # i2c_master_bus_config_t uses union: clk_source vs lp_source_clk (SOC_LP_I2C_SUPPORTED)
        if isinstance(i2c_port, str) and i2c_port.startswith('LP_I2C_NUM_'):
            struct_init['lp_source_clk'] = clk_source
        else:
            struct_init['clk_source'] = clk_source

        return {
            'struct_type': 'i2c_master_bus_config_t',
            'struct_var': f'{name}_cfg',
            'struct_init': struct_init,
        }

    except ValueError as e:
        # Re-raise ValueError with more context
        raise ValueError(f"YAML validation error in I2C peripheral '{name}': {e}")
    except Exception as e:
        # Catch any other exceptions and provide context
        raise ValueError(f"Error parsing I2C peripheral '{name}': {e}")
