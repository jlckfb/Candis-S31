# SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

# Power control device config parser
VERSION = 'v1.0.0'

import sys

from devices.dev_custom import dev_custom

def get_includes():
    """Get required header includes for power control device"""
    return ['dev_power_ctrl.h']

# power_ctrl sub_types dispatched via the generic '<sub_type>_power_ctrl' extra-func key
# (see dev_power_ctrl_sub_gpio.c / dev_power_ctrl_sub_custom.c). A 'custom' power_ctrl
# device registers its board-specific ops under its own device name in the same
# extra-func registry, so a device name equal to one of these reserved keys would
# silently shadow (or be shadowed by) the framework's own dispatch entry.
_RESERVED_POWER_CTRL_SUB_TYPES = ('gpio', 'custom')
_RESERVED_POWER_CTRL_NAMES = {f'{sub}_power_ctrl' for sub in _RESERVED_POWER_CTRL_SUB_TYPES}

def parse(name, config, peripherals_dict=None):
    """
    Parse power control device configuration from YAML to C structure

    Args:
        name (str): Device name
        config (dict): Device configuration dictionary
        peripherals_dict (dict): Dictionary of available peripherals for validation

    Returns:
        dict: Parsed configuration with 'struct_type' and 'struct_init' keys
    """
    # Extract configuration parameters
    device_config = config.get('config', {})
    sub_type = config.get('sub_type', '')

    if not sub_type:
        raise ValueError(f"Power control device '{name}' missing required field 'sub_type'")

    # Build base structure initialization
    struct_init = {
        'name': name,
        'sub_type': sub_type,
    }

    # Handle different sub_types
    if sub_type == 'gpio':
        # Get peripherals from device_config
        peripherals = device_config.get('peripherals', [])
        if not peripherals:
            peripherals = config.get('peripherals', [])
            if not peripherals:
                raise ValueError(f"GPIO power control device '{name}' missing required field 'peripherals'")

        # Find the GPIO peripheral
        gpio_peripheral = None
        for periph in peripherals:
            if isinstance(periph, dict) and 'name' in periph:
                # Check if this peripheral is a GPIO peripheral
                periph_name = periph.get('name', '')
                if peripherals_dict and periph_name in peripherals_dict:
                    periph_obj = peripherals_dict[periph_name]
                    if hasattr(periph_obj, 'type') and periph_obj.type == 'gpio':
                        gpio_peripheral = periph
                        break

        if not gpio_peripheral:
            raise ValueError(f"GPIO power control device '{name}' missing GPIO peripheral in peripherals list")

        # Get GPIO peripheral name
        gpio_periph_name = gpio_peripheral.get('name')
        # Get active_level from peripheral configuration
        active_level = gpio_peripheral.get('active_level', 1)  # Default to active high
        if active_level not in [0, 1]:
            raise ValueError(f"GPIO power control device '{name}' has invalid active_level: {active_level}. Must be 0 or 1")

        # Set the gpio member of the union
        sub_cfg_union = {
            'gpio': {
                'gpio_name': gpio_periph_name,
                'active_level': active_level
            }
        }
        struct_init['sub_cfg'] = sub_cfg_union

    elif sub_type == 'custom':
        if name in _RESERVED_POWER_CTRL_NAMES:
            raise ValueError(
                f"Custom power control device name '{name}' collides with the internal "
                f"power_ctrl dispatch key '{name}'; rename the device to avoid overriding "
                f"the framework's own '<sub_type>_power_ctrl' extra-func registration"
            )

        peripherals = config.get('peripherals', [])
        periph_names = []
        for periph in peripherals:
            if isinstance(periph, dict):
                periph_name = periph.get('name')
            else:
                periph_name = periph
            if not periph_name:
                raise ValueError(f"Custom power control device '{name}' has a peripheral entry without name")
            if peripherals_dict is not None and periph_name not in peripherals_dict:
                raise ValueError(f"Custom power control device '{name}' references unknown peripheral '{periph_name}'")
            periph_names.append(periph_name)

        periph_array_var = f'esp_bmgr_{name.replace("-", "_")}_periph_names'
        extra_configs = []
        if periph_names:
            extra_configs.append({
                'struct_type': 'const char *',
                'struct_var': f'{periph_array_var}[]',
                'struct_init': periph_names,
            })

        user_config = config.get('config', {})
        user_config_result = None
        user_config_var = None
        if user_config:
            user_config_name = f'{name}_custom'
            user_config_result = dev_custom.parse(
                user_config_name,
                {
                    'type': 'custom',
                    'config': user_config,
                },
                peripherals_dict={},
            )
            user_config_var = f'esp_bmgr_{name.replace("-", "_")}_custom_cfg'
            extra_configs.append({
                'struct_type': user_config_result['struct_type'],
                'struct_var': user_config_var,
                'struct_init': user_config_result['struct_init'],
            })

        struct_init['sub_cfg'] = {
            'custom': {
                'periph_names': periph_array_var if periph_names else None,
                'periph_count': len(periph_names),
                'user_cfg': f'&{user_config_var}' if user_config_var else None,
                'user_cfg_size': f'sizeof({user_config_var})' if user_config_var else 0,
            }
        }

    else:
        raise ValueError(f"Unsupported power control sub_type '{sub_type}' for device '{name}'")

    result = {
        'struct_type': 'dev_power_ctrl_config_t',
        'struct_var': f'{name}_cfg',
        'struct_init': struct_init
    }
    if sub_type == 'custom' and extra_configs:
        result['extra_configs'] = extra_configs
    if sub_type == 'custom' and user_config_result:
        result['struct_definition'] = user_config_result['struct_definition']
    return result
