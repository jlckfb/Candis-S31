# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

"""
Configuration utilities for ESP Board Manager.

Handles deep merging of board YAMLs and applying ``-a/--amend`` plans on top of
a base ``board_devices.yaml`` / ``board_peripherals.yaml``.
"""

from typing import Any, Callable, Dict, List, Optional, TYPE_CHECKING

from .yaml_utils import load_yaml_mapping_from_text

if TYPE_CHECKING:  # pragma: no cover - import-only typing
    from ..amend import AmendPlan


def deep_merge_config(base: dict, override: dict) -> dict:
    """
    Deep merge override configuration into base configuration.
    Override values take precedence over base values.

    Args:
        base: Base configuration dictionary
        override: Override configuration dictionary

    Returns:
        Merged configuration dictionary
    """
    result = base.copy()
    for key, value in override.items():
        if key in result and isinstance(result[key], dict) and isinstance(value, dict):
            # Recursively merge nested dictionaries
            result[key] = deep_merge_config(result[key], value)
        else:
            # For non-dict values (including lists), override completely replaces base
            result[key] = value
    return result


def apply_named_list_overrides(
    base_items: List[dict],
    override_items: List[dict],
    logger,
    *,
    item_label: str,
    update_existing: Callable[[dict, dict], None],
    context: str = '',
) -> List[dict]:
    """Apply override list entries by matching the ``name`` field."""
    item_map = {
        item['name']: idx
        for idx, item in enumerate(base_items)
        if isinstance(item, dict) and 'name' in item
    }
    context_suffix = f' {context}' if context else ''

    for override_item in override_items:
        if not isinstance(override_item, dict):
            logger.warning(f'⚠️  Override {item_label}{context_suffix} is not a dict, skipping: {override_item}')
            continue

        if 'name' not in override_item:
            logger.warning(f'⚠️  Override {item_label}{context_suffix} missing name field, skipping')
            continue

        item_name = override_item['name']
        if item_name not in item_map:
            base_items.append(override_item)
            item_map[item_name] = len(base_items) - 1
            logger.debug(f'   Added override {item_label}: {item_name}')
            continue

        base_item = base_items[item_map[item_name]]
        update_existing(base_item, override_item)
        logger.debug(f'   Applied overrides to {item_label}: {item_name}')

    return base_items


def apply_peripheral_overrides(base_periphs: List, override_periphs: List, logger, device_name: str) -> List:
    """
    Apply override configurations to peripheral configurations within a device.
    Matches peripherals by 'name' field; appends new peripheral references.
    """
    def update_existing(base_periph: dict, override_periph: dict) -> None:
        for key, value in override_periph.items():
            if key == 'name':
                continue
            base_periph[key] = value

    return apply_named_list_overrides(
        base_periphs,
        override_periphs,
        logger,
        item_label='peripheral',
        update_existing=update_existing,
        context=f"for device '{device_name}'",
    )


def apply_device_overrides(base_devices: List[dict], override_devices: List[dict], logger) -> List[dict]:
    """
    Apply override configurations to base device configurations.
    Matches devices by 'name' field and merges their configurations; appends new devices.
    """
    # Create a mapping of device names to base devices
    device_map = {dev['name']: dev for dev in base_devices if 'name' in dev}

    # Apply overrides
    for override_dev in override_devices:
        if 'name' not in override_dev:
            logger.warning(f"⚠️  Override device missing 'name' field, skipping: {override_dev}")
            continue

        dev_name = override_dev['name']
        if dev_name not in device_map:
            base_devices.append(override_dev)
            device_map[dev_name] = override_dev
            logger.debug(f'   Added override device: {dev_name}')
            continue

        base_dev = device_map[dev_name]

        # Merge device-level fields
        for key, value in override_dev.items():
            if key == 'name':
                continue  # Don't override the name itself
            elif key == 'config' and 'config' in base_dev:
                # Deep merge config dictionaries
                base_dev['config'] = deep_merge_config(base_dev['config'], value)
            elif key == 'peripherals' and 'peripherals' in base_dev:
                # Merge peripherals by name
                base_dev['peripherals'] = apply_peripheral_overrides(
                    base_dev['peripherals'], value, logger, dev_name
                )
            else:
                # For other fields (init_skip, chip, version, sub_type, etc.), override directly
                base_dev[key] = value

        logger.debug(f'   Applied overrides to device: {dev_name}')

    return base_devices


def apply_peripheral_list_overrides(base_periphs: List, override_periphs: List, logger) -> List:
    """
    Apply override configurations to top-level peripheral list (from board_peripherals.yaml).
    Matches peripherals by 'name' field; appends new peripherals.
    """
    def update_existing(base_periph: dict, override_periph: dict) -> None:
        for key, value in override_periph.items():
            if key == 'name':
                continue
            elif key == 'config' and 'config' in base_periph:
                base_periph['config'] = deep_merge_config(base_periph['config'], value)
            else:
                base_periph[key] = value

    return apply_named_list_overrides(
        base_periphs,
        override_periphs,
        logger,
        item_label='peripheral',
        update_existing=update_existing,
    )


def load_yaml_with_includes(
    yaml_path: str,
    amend_plan: Optional['AmendPlan'] = None,
) -> Optional[Dict[str, Any]]:
    """Load one board YAML file and optionally apply an amend plan on top.

    The amend plan's YAML fragments are merged into the loaded data in manifest
    order: items with the same ``name`` are merged field-by-field, new names are
    appended. See ``docs/zh_CN/create-board/amend.rst`` for full semantics.

    Args:
        yaml_path:  path to ``board_devices.yaml`` / ``board_peripherals.yaml``.
        amend_plan: optional :class:`generators.amend.AmendPlan` to apply.

    Returns:
        Parsed and merged mapping, or ``None`` if the file is missing data
        (current callers treat ``None`` and ``{}`` equivalently).
    """
    import os

    if not os.path.exists(yaml_path):
        raise FileNotFoundError(yaml_path)

    try:
        with open(yaml_path, 'r', encoding='utf-8') as f:
            main_content = f.read()
    except Exception as e:
        print(f'Error: Cannot read file! Path: {yaml_path}')
        print(f'Error: {e}')
        raise

    base_data = load_yaml_mapping_from_text(main_content, yaml_path)

    if amend_plan is None:
        return base_data

    # Local import to avoid an import cycle (amend imports config_utils helpers).
    from ..amend import apply_amend_plan_to_yaml
    from pathlib import Path

    return apply_amend_plan_to_yaml(base_data, amend_plan, base_path=Path(yaml_path))
