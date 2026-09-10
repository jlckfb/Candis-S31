# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
# SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
#
# See LICENSE file for details.

"""
Schema validator for ESP Board Manager
Validates device configurations against device template schemas
"""

from pathlib import Path
from typing import Dict, List, Optional, Set, Any
from dataclasses import dataclass
import yaml
import difflib

from .utils.logger import LoggerMixin


@dataclass
class ValidationIssue:
    """Represents a validation issue found in configuration"""
    level: str  # 'ERROR', 'WARNING', 'INFO'
    message: str
    device_name: str
    key_path: str
    suggestion: Optional[str] = None


class DeviceSchemaValidator(LoggerMixin):
    """Validator for device configurations against template schemas"""

    _TYPE_WIDE_KEY = '__type_wide__'  # sentinel used for the cross-sub_type union

    def __init__(self, devices_dir: Path):
        super().__init__()
        self.devices_dir = devices_dir
        # Cache: {(device_type, sub_type): schema, (device_type, '__type_wide__'): union}
        self.schemas = {}

    def load_schema(self, device_type: str, sub_type: Optional[str] = None) -> Optional[Set[str]]:
        """
        Load schema from device template YAML file.

        Returns the *flat* set of valid config keys (including nested keys in
        dot notation) for ``(device_type, sub_type)``. When the requested
        ``sub_type`` has no entry in the template, falls back to the entry
        without an explicit ``sub_type`` (legacy templates).

        A type-wide cache entry ``(device_type, None)`` is always populated as
        the union of every sub_type's keys, mirroring how PeripheralSchemaValidator
        keeps a ``(periph_type, None)`` union. :meth:`load_type_wide_schema`
        gives direct access to it.

        Returns ``None`` if the template file does not exist or fails to parse.
        """
        cache_key = (device_type, sub_type)
        if cache_key in self.schemas:
            return self.schemas[cache_key]

        # Find template file
        template_path = self.devices_dir / f'dev_{device_type}' / f'dev_{device_type}.yaml'

        if not template_path.exists():
            self.logger.debug(f'   No template file found for device type: {device_type}')
            return None

        try:
            with open(template_path, 'r', encoding='utf-8') as f:
                template_data = yaml.safe_load(f)

            if not template_data or not isinstance(template_data, list):
                self.logger.debug(f'   Invalid template format: {template_path}')
                return None

            # Extract schemas for all sub_types
            schemas = self._extract_schemas_from_template(template_data)
            self.logger.debug(f'   Successfully loaded schema for {device_type} from {template_path}')

            # Cache per-sub_type schemas (key may be None for legacy templates
            # whose example entry omits the sub_type field).
            for st, schema in schemas.items():
                self.schemas[(device_type, st)] = schema

            # Cache the type-wide union under a separate sentinel key so it
            # never clashes with a legacy sub_type=None entry. Used as the
            # forgiving fallback in validate_config().
            type_wide = set()
            for schema in schemas.values():
                type_wide.update(schema)
            self.schemas[(device_type, self._TYPE_WIDE_KEY)] = type_wide

            if sub_type in schemas:
                return schemas[sub_type]
            # Legacy templates have an entry with no sub_type field; honour it.
            return schemas.get(None, type_wide)

        except Exception as e:
            self.logger.debug(f'   Error loading template {template_path}: {e}')
            return None

    def load_type_wide_schema(self, device_type: str) -> Optional[Set[str]]:
        """Return the union of every sub_type's config keys for ``device_type``.

        Used by :meth:`validate_config` to keep cross-sub_type leftovers from
        showing up as user-facing warnings — they are demoted to debug logs.
        """
        # Force population of the union if needed.
        self.load_schema(device_type)
        return self.schemas.get((device_type, self._TYPE_WIDE_KEY))

    def _extract_schemas_from_template(self, template_data: List[Dict]) -> Dict[Optional[str], Set[str]]:
        """
        Extract schemas from template data

        Args:
            template_data: List of device configurations from template

        Returns:
            Dictionary mapping sub_types to sets of valid config keys
        """
        schemas = {}

        for device_config in template_data:
            if not isinstance(device_config, dict):
                continue

            sub_type = device_config.get('sub_type')
            config = device_config.get('config', {})

            if not isinstance(config, dict):
                continue

            # Extract all keys recursively
            keys = self._extract_keys_recursive(config)

            if sub_type not in schemas:
                schemas[sub_type] = set()

            schemas[sub_type].update(keys)

        return schemas

    def _extract_keys_recursive(self, config: Dict, prefix: str = '') -> Set[str]:
        """
        Recursively extract all keys from a configuration dictionary

        Args:
            config: Configuration dictionary
            prefix: Prefix for nested keys (e.g., 'events_cfg.')

        Returns:
            Set of all keys (including nested keys with dot notation)
        """
        keys = set()

        for key, value in config.items():
            full_key = f'{prefix}{key}' if prefix else key
            keys.add(full_key)

            # Recursively extract nested keys
            if isinstance(value, dict):
                nested_keys = self._extract_keys_recursive(value, f'{full_key}.')
                keys.update(nested_keys)

        return keys

    def validate_device_fields(self, device_dict: Dict[str, Any], device_name: str) -> List[ValidationIssue]:
        """
        Validate top-level fields of a device dictionary
        """
        issues = []
        allowed_fields = {
            'name', 'type', 'sub_type', 'version', 'chip', 'config',
            'peripherals', 'dependencies', 'depends_on', 'init_skip', 'gen_skip', 'power_ctrl_device'
        }

        for key in device_dict.keys():
            if key not in allowed_fields:
                # Find similar valid key
                suggestion = difflib.get_close_matches(key, list(allowed_fields), n=1, cutoff=0.6)
                suggestion = suggestion[0] if suggestion else None

                message = f"Device '{device_name}': Unknown field '{key}'"

                # Special case for common typo
                if key == 'cfg' and not suggestion:
                    suggestion = 'config'

                issue = ValidationIssue(
                    level='WARNING',
                    message=message,
                    device_name=device_name,
                    key_path=key,
                    suggestion=suggestion
                )
                issues.append(issue)

        return issues

    def validate_config(self, device_type: str, sub_type: Optional[str], config: Dict[str, Any],
                       device_name: str) -> List[ValidationIssue]:
        """
        Validate device configuration against schema.

        Two-tier policy (mirrors the forgiving behaviour of
        :class:`PeripheralSchemaValidator`):

        - **type-wide union** is the source of truth for "is this key known at
          all". Any key not in the union becomes a WARNING with a fuzzy
          suggestion.
        - **current sub_type's strict schema** is used only to log debug
          messages for keys that belong to a *different* sub_type — typical
          residue when an amend swaps ``sub_type``. These never escalate to
          warnings, but ``--log-level DEBUG`` will surface them.

        This intentionally drops the previous "strict per-sub_type" warning
        behaviour: it caused noisy false positives whenever an amend changed
        ``sub_type`` (the merged config legally carried the previous
        sub_type's namespace blocks, which the new sub_type's parser simply
        ignores). The forgiving policy keeps real typos visible while
        treating cross-sub_type leftovers as a non-event.
        """
        issues = []

        if device_type == 'custom' or (device_type == 'power_ctrl' and sub_type == 'custom'):
            return issues

        type_wide = self.load_type_wide_schema(device_type)
        if type_wide is None:
            # No schema available at all — preserve the legacy "skip silently"
            # behaviour so undocumented device types do not crash here.
            return issues

        strict_sub = self.load_schema(device_type, sub_type)

        config_keys = self._extract_keys_recursive(config)

        for key in config_keys:
            if self._should_ignore_key(key):
                continue

            if key in type_wide:
                # The key is recognised by *some* sub_type's schema. If it does
                # not belong to the current sub_type's strict schema, note it
                # at debug level so authors can still catch a cross-sub_type
                # typo with --log-level DEBUG; never warn the user, because
                # the device parser will simply ignore the field at runtime.
                if strict_sub is not None and key not in strict_sub:
                    self.logger.debug(
                        f"   Device '{device_name}' ({device_type}): config key "
                        f"'{key}' is defined in another sub_type, not "
                        f"'{sub_type}'. Ignored by this device's parser."
                    )
                continue

            # Genuinely unknown key (no sub_type defines it). Warn + suggest.
            suggestion = self._find_similar_key(key, type_wide)

            issues.append(ValidationIssue(
                level='WARNING',
                message=f"Device '{device_name}' ({device_type}): Unknown config key '{key}'",
                device_name=device_name,
                key_path=key,
                suggestion=suggestion,
            ))

        return issues

    def _should_ignore_key(self, key: str) -> bool:
        """
        Check if a key should be ignored during validation

        Some keys are dynamic or list-based and shouldn't be validated
        """
        # Ignore numeric indices (for lists)
        parts = key.split('.')
        for part in parts:
            if part.isdigit():
                return True

        return False

    def _find_similar_key(self, invalid_key: str, valid_keys: Set[str]) -> Optional[str]:
        """
        Find similar valid key using fuzzy matching

        Args:
            invalid_key: The invalid key to match
            valid_keys: Set of valid keys

        Returns:
            Most similar valid key, or None if no good match found
        """
        # Extract the base key (without nested path)
        base_invalid = invalid_key.split('.')[-1]

        # Find close matches
        close_matches = difflib.get_close_matches(base_invalid,
                                                   [k.split('.')[-1] for k in valid_keys],
                                                   n=1, cutoff=0.6)

        if close_matches:
            # Find the full key that matches
            for valid_key in valid_keys:
                if valid_key.endswith(close_matches[0]):
                    return valid_key

        # Try case-insensitive match
        for valid_key in valid_keys:
            if valid_key.lower() == invalid_key.lower():
                return valid_key

        return None

    def format_issue(self, issue: ValidationIssue) -> str:
        """
        Format a validation issue for display

        Args:
            issue: ValidationIssue to format

        Returns:
            Formatted message string
        """
        msg = f'⚠️  {issue.message}'

        if issue.suggestion:
            msg += f"\n   💡 Did you mean '{issue.suggestion}'?"

        return msg


class PeripheralSchemaValidator(LoggerMixin):
    """Validator for peripheral configurations against template schemas"""

    def __init__(self, peripherals_dir: Path):
        super().__init__()
        self.peripherals_dir = peripherals_dir
        self.schemas = {}  # Cache for loaded schemas: {(periph_type, role): schema}

    def load_schema(self, periph_type: str, role: Optional[str] = None) -> Optional[Set[str]]:
        """
        Load schema from peripheral template YAML file

        Args:
            periph_type: Type of peripheral (e.g., 'i2c', 'spi', 'gpio')
            role: Optional role of peripheral (e.g., 'continuous', 'oneshot')

        Returns:
            Set of valid config keys, or None if template file not found
        """
        cache_key = (periph_type, role)
        if cache_key in self.schemas:
            return self.schemas[cache_key]

        # Find template file
        template_path = self.peripherals_dir / f'periph_{periph_type}' / f'periph_{periph_type}.yml'

        if not template_path.exists():
            self.logger.debug(f'   No template file found for peripheral type: {periph_type}')
            return None

        try:
            with open(template_path, 'r', encoding='utf-8') as f:
                template_data = yaml.safe_load(f)

            if not template_data or not isinstance(template_data, list):
                self.logger.debug(f'   Invalid template format: {template_path}')
                return None

            # Extract schemas for all roles plus a type-wide fallback.
            schemas = self._extract_schemas_from_template(template_data)
            self.logger.debug(f'   Successfully loaded schema for {periph_type} from {template_path}')

            all_schema = set()
            for schema in schemas.values():
                all_schema.update(schema)

            # Cache role-specific schemas and the type-wide fallback.
            for schema_role, schema in schemas.items():
                self.schemas[(periph_type, schema_role)] = schema
            self.schemas[(periph_type, None)] = all_schema

            if role in schemas:
                return schemas[role]
            return all_schema

        except Exception as e:
            self.logger.debug(f'   Error loading template {template_path}: {e}')
            return None

    def _extract_schemas_from_template(self, template_data: List[Dict]) -> Dict[Optional[str], Set[str]]:
        """
        Extract schemas from template data

        Args:
            template_data: List of peripheral configurations from template

        Returns:
            Dictionary mapping roles to valid config keys
        """
        schemas = {}

        for periph_config in template_data:
            if not isinstance(periph_config, dict):
                continue

            role = periph_config.get('role')
            config = periph_config.get('config', {})

            if not isinstance(config, dict):
                continue

            # Extract all keys recursively
            config_keys = self._extract_keys_recursive(config)
            if role not in schemas:
                schemas[role] = set()
            schemas[role].update(config_keys)

        return schemas

    def _extract_keys_recursive(self, config: Dict, prefix: str = '') -> Set[str]:
        """
        Recursively extract all keys from a configuration dictionary

        Args:
            config: Configuration dictionary
            prefix: Prefix for nested keys (e.g., 'pins.')

        Returns:
            Set of all keys (including nested keys with dot notation)
        """
        keys = set()

        for key, value in config.items():
            full_key = f'{prefix}{key}' if prefix else key
            keys.add(full_key)

            # Recursively extract nested keys
            if isinstance(value, dict):
                nested_keys = self._extract_keys_recursive(value, f'{full_key}.')
                keys.update(nested_keys)

        return keys

    def validate_peripheral_fields(self, periph_dict: Dict[str, Any], periph_name: str) -> List[ValidationIssue]:
        """
        Validate top-level fields of a peripheral dictionary
        """
        issues = []
        allowed_fields = {
            'name', 'type', 'config', 'role', 'format', 'version', 'gen_skip'
        }

        for key in periph_dict.keys():
            if key not in allowed_fields:
                suggestion = self._find_suggestion(key, allowed_fields)
                issues.append(ValidationIssue(
                    level='WARNING',
                    message=f"Peripheral '{periph_name}': Unknown field '{key}'",
                    device_name=periph_name,
                    key_path=key,
                    suggestion=suggestion
                ))

        return issues

    def validate_config(self, periph_type: str, config: Dict[str, Any], periph_name: str,
                        role: Optional[str] = None) -> List[ValidationIssue]:
        """
        Validate peripheral configuration against schema.

        Two-tier policy (mirrors :meth:`DeviceSchemaValidator.validate_config`):

        - **type-wide union** (``schemas[(periph_type, None)]``, already
          maintained by :meth:`load_schema`) decides whether a key is known
          at all. Unknown keys become WARNINGs with a fuzzy suggestion.
        - **role-specific strict schema** is consulted only to log a debug
          message for keys that belong to a *different* role — useful when
          an amend swaps ``role`` (e.g. ADC ``continuous`` ↔ ``oneshot``)
          and the merged config legally carries the previous role's fields,
          which the new role's parser simply ignores.
        """
        issues = []

        # Force the type-wide union to be populated, then read it.
        self.load_schema(periph_type, None)
        type_wide = self.schemas.get((periph_type, None))
        if type_wide is None:
            # No schema available at all — preserve legacy "skip silently".
            return issues

        # role-specific strict schema (may be None for legacy templates or
        # peripherals without a role concept).
        strict_role = None
        if role is not None:
            strict_role = self.schemas.get((periph_type, role))

        config_keys = self._extract_keys_recursive(config)

        # Historical exemption: SPI bus has data0..data3_io_num that appear in
        # SDK headers but not in our example template.
        allowed_extra_keys = set()
        if periph_type == 'spi':
            allowed_extra_keys = {
                'spi_bus_config.data0_io_num',
                'spi_bus_config.data1_io_num',
                'spi_bus_config.data2_io_num',
                'spi_bus_config.data3_io_num',
            }

        for key in config_keys:
            if key in allowed_extra_keys:
                continue

            if key in type_wide:
                if strict_role is not None and key not in strict_role:
                    self.logger.debug(
                        f"   Peripheral '{periph_name}' ({periph_type}): config "
                        f"key '{key}' is defined for another role, not "
                        f"'{role}'. Ignored by this peripheral's parser."
                    )
                continue

            # Genuinely unknown — not part of any role's schema.
            suggestion = self._find_suggestion(key, type_wide)
            issues.append(ValidationIssue(
                level='WARNING',
                message=f"Peripheral '{periph_name}' ({periph_type}): Unknown config key '{key}'",
                device_name=periph_name,
                key_path=key,
                suggestion=suggestion,
            ))

        return issues

    def _find_suggestion(self, invalid_key: str, valid_keys: Set[str]) -> Optional[str]:
        """
        Find a suggested correction for an invalid key using fuzzy matching

        Args:
            invalid_key: The invalid key
            valid_keys: Set of valid keys

        Returns:
            Suggested key, or None if no good match found
        """
        # Try exact match on base name (last component after last dot)
        base_invalid = invalid_key.split('.')[-1]
        close_matches = difflib.get_close_matches(base_invalid,
                                                   [k.split('.')[-1] for k in valid_keys],
                                                   n=1, cutoff=0.6)

        if close_matches:
            # Find the full key that matches
            for valid_key in valid_keys:
                if valid_key.endswith(close_matches[0]):
                    return valid_key

        # Try case-insensitive match
        for valid_key in valid_keys:
            if valid_key.lower() == invalid_key.lower():
                return valid_key

        return None

    def format_issue(self, issue: ValidationIssue) -> str:
        """
        Format a validation issue for display

        Args:
            issue: ValidationIssue to format

        Returns:
            Formatted message string
        """
        msg = f'⚠️  {issue.message}'

        if issue.suggestion:
            msg += f"\n   💡 Did you mean '{issue.suggestion}'?"

        return msg
