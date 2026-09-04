# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

# LittleFS device config parser
VERSION = 'v1.0.0'

LITTLEFS_SUB_TYPES = ['flash', 'sdmmc', 'spi']

DEV_LITTLEFS_IO_LIST = {
    'sdmmc': [
        'clk',
        'cmd',
        'd0',
        'd1',
        'd2',
        'd3',
        'd4',
        'd5',
        'd6',
        'd7',
        'cd',
        'wp',
    ],
    'spi': [
        'cs_gpio_num',
    ],
}

VALID_SDMMC_ENUMS = {
    'slot': [
        'SDMMC_HOST_SLOT_0',
        'SDMMC_HOST_SLOT_1',
        'SDMMC_HOST_SLOT_2',
    ],
    'frequency': [
        'SDMMC_FREQ_DEFAULT',
        'SDMMC_FREQ_HIGHSPEED',
        'SDMMC_FREQ_PROBING',
        'SDMMC_FREQ_52M',
        'SDMMC_FREQ_26M',
        'SDMMC_FREQ_DDR50',
        'SDMMC_FREQ_SDR50',
    ],
    'slot_flags': [
        0,
        'SDMMC_SLOT_FLAG_INTERNAL_PULLUP',
        'SDMMC_SLOT_FLAG_WP_ACTIVE_HIGH',
        'SDMMC_SLOT_FLAG_UHS1',
    ],
}


def validate_enum_value(value, enum_type: str) -> bool:
    """Validate ESP-IDF SDMMC enum values used by LittleFS SD backends."""
    if enum_type not in VALID_SDMMC_ENUMS:
        return True
    if value not in VALID_SDMMC_ENUMS[enum_type]:
        raise ValueError(
            f"YAML validation error in LittleFS device: Invalid {enum_type} value '{value}'. "
            f'Valid values: {VALID_SDMMC_ENUMS.get(enum_type, [])}'
        )
    return True


def get_enum_value(value, default_value, enum_type: str = None):
    """Return YAML enum value or default value after validation."""
    if value is None or value == '':
        result = default_value
    elif enum_type == 'slot_flags' and value == '0':
        result = 0
    else:
        result = value

    if enum_type:
        validate_enum_value(result, enum_type)
    return result


def get_includes() -> list:
    """Return required include headers for LittleFS device configuration."""
    return [
        'dev_littlefs.h',
        'driver/sdmmc_host.h',
        'sdmmc_cmd.h',
    ]


def parse_littlefs_vfs_config(vfs_config: dict, sub_type: str) -> dict:
    """Parse esp_vfs_littlefs_conf_t fields that can be safely generated from YAML."""
    runtime_fields = {'partition', 'sdcard', 'blockdev'}
    present_runtime_fields = sorted(runtime_fields.intersection(vfs_config.keys()))
    if present_runtime_fields:
        raise ValueError(
            'LittleFS vfs_config does not accept runtime handle fields from YAML: '
            + ', '.join(present_runtime_fields)
        )

    read_only = bool(vfs_config.get('read_only', False))
    format_if_mount_failed = bool(vfs_config.get('format_if_mount_failed', False))
    grow_on_mount = bool(vfs_config.get('grow_on_mount', False))
    if read_only and format_if_mount_failed:
        raise ValueError('LittleFS read_only cannot be used with format_if_mount_failed')
    if read_only and grow_on_mount:
        raise ValueError('LittleFS read_only cannot be used with grow_on_mount')

    partition_label = vfs_config.get('partition_label')
    if sub_type == 'flash':
        partition_label = partition_label if partition_label is not None else 'storage'
    else:
        partition_label = None

    return {
        'base_path': vfs_config.get('base_path', '/littlefs'),
        'partition_label': partition_label,
        'partition': None,
        'format_if_mount_failed': format_if_mount_failed,
        'read_only': read_only,
        'dont_mount': bool(vfs_config.get('dont_mount', False)),
        'grow_on_mount': grow_on_mount,
    }


def parse_sdmmc_sub_config(sub_config: dict) -> dict:
    """Parse SDMMC backend configuration."""
    pins = sub_config.get('pins', {})
    return {
        'slot': get_enum_value(sub_config.get('slot', 'SDMMC_HOST_SLOT_1'), 'SDMMC_HOST_SLOT_1', 'slot'),
        'bus_width': sub_config.get('bus_width', 1),
        'slot_flags': get_enum_value(
            sub_config.get('slot_flags', 'SDMMC_SLOT_FLAG_INTERNAL_PULLUP'),
            'SDMMC_SLOT_FLAG_INTERNAL_PULLUP',
            'slot_flags',
        ),
        'pins': {
            'clk': pins.get('clk', -1),
            'cmd': pins.get('cmd', -1),
            'd0': pins.get('d0', -1),
            'd1': pins.get('d1', -1),
            'd2': pins.get('d2', -1),
            'd3': pins.get('d3', -1),
            'd4': pins.get('d4', -1),
            'd5': pins.get('d5', -1),
            'd6': pins.get('d6', -1),
            'd7': pins.get('d7', -1),
            'cd': pins.get('cd', -1),
            'wp': pins.get('wp', -1),
        },
        'ldo_chan_id': sub_config.get('ldo_chan_id', -1),
    }


def parse_spi_sub_config(sub_config: dict, device_peripherals: list = None, peripherals_dict=None) -> dict:
    """Parse SDSPI backend configuration and resolve the referenced SPI peripheral."""
    cs_gpio_num = sub_config.get('cs_gpio_num', 15)
    spi_bus_name = None
    device_peripherals = device_peripherals or []

    if peripherals_dict is None:
        raise ValueError(
            'LittleFS device with spi sub_type requires peripheral validation context '
            'to resolve a SPI master peripheral reference'
        )

    if not device_peripherals:
        raise ValueError(
            'LittleFS device with spi sub_type requires a SPI master peripheral reference '
            'in device-level peripherals'
        )

    checked_peripherals = []
    missing_peripherals = []

    for peripheral in device_peripherals:
        periph_name = peripheral.get('name') if isinstance(peripheral, dict) else peripheral
        if not periph_name:
            checked_peripherals.append('<missing name>')
            continue
        periph_obj = peripherals_dict.get(periph_name)
        if not periph_obj:
            missing_peripherals.append(periph_name)
            continue
        periph_type = getattr(periph_obj, 'type', None)
        periph_role = getattr(periph_obj, 'role', None)
        checked_peripherals.append(f'{periph_name}(type={periph_type}, role={periph_role})')
        if periph_type == 'spi' and periph_role == 'master':
            spi_bus_name = periph_name
            break

    if not spi_bus_name:
        details = []
        if missing_peripherals:
            details.append(f'missing references: {missing_peripherals}')
        if checked_peripherals:
            details.append(f'checked references: {checked_peripherals}')
        detail_text = '; ' + '; '.join(details) if details else ''
        raise ValueError(
            'LittleFS device with spi sub_type requires a SPI master peripheral reference '
            f'at device level{detail_text}'
        )

    return {
        'cs_gpio_num': cs_gpio_num,
        'spi_bus_name': spi_bus_name,
    }


def parse(name: str, full_config: dict, peripherals_dict=None) -> dict:
    """Parse LittleFS device configuration from board_devices.yaml."""
    config = full_config.get('config', {})
    sub_type = full_config.get('sub_type')
    if not sub_type:
        raise ValueError(f"LittleFS device '{name}' is missing required 'sub_type' field")
    if sub_type not in LITTLEFS_SUB_TYPES:
        raise ValueError(
            f"LittleFS device '{name}' has invalid 'sub_type' value '{sub_type}'. "
            f'Must be one of {LITTLEFS_SUB_TYPES}'
        )

    vfs_config = parse_littlefs_vfs_config(config.get('vfs_config', {}), sub_type)
    sub_config = config.get('sub_config', {})
    device_peripherals = full_config.get('peripherals', [])

    struct_init = {
        'name': name,
        'sub_type': sub_type,
        'vfs_config': vfs_config,
        'frequency': 0,
    }

    if sub_type == 'sdmmc':
        struct_init['frequency'] = get_enum_value(
            sub_config.get('frequency', 'SDMMC_FREQ_HIGHSPEED'),
            'SDMMC_FREQ_HIGHSPEED',
            'frequency',
        )
        struct_init['sub_cfg'] = {
            'sdmmc': parse_sdmmc_sub_config(sub_config),
        }
    elif sub_type == 'spi':
        struct_init['frequency'] = get_enum_value(
            sub_config.get('frequency', 'SDMMC_FREQ_DEFAULT'),
            'SDMMC_FREQ_DEFAULT',
            'frequency',
        )
        struct_init['sub_cfg'] = {
            'spi': parse_spi_sub_config(sub_config, device_peripherals, peripherals_dict),
        }

    return {
        'struct_type': 'dev_littlefs_config_t',
        'struct_var': f'{name}_cfg',
        'struct_init': struct_init,
    }
