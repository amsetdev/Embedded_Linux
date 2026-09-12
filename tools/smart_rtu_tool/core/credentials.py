"""
credentials.py
---------------
Thin wrapper around the `keyring` library so MQTT/SSH passwords are never
hardcoded in the tool or written to a plaintext config file. Uses the
OS-native secure store (Windows Credential Manager / macOS Keychain /
Linux Secret Service).

Field technicians enter credentials once; subsequent launches read them
back from the OS store silently.
"""

from __future__ import annotations
from typing import Optional
import keyring

SERVICE_NAME = "smart_rtu_tool"


def save_secret(key: str, value: str):
    keyring.set_password(SERVICE_NAME, key, value)


def get_secret(key: str) -> Optional[str]:
    return keyring.get_password(SERVICE_NAME, key)


def delete_secret(key: str):
    try:
        keyring.delete_password(SERVICE_NAME, key)
    except keyring.errors.PasswordDeleteError:
        pass


# Convenience wrappers for the specific secrets this tool manages
def save_mqtt_password(pw: str):
    save_secret("mqtt_password", pw)


def get_mqtt_password() -> Optional[str]:
    return get_secret("mqtt_password")


def save_ssh_password(pw: str):
    save_secret("ssh_password", pw)


def get_ssh_password() -> Optional[str]:
    return get_secret("ssh_password")


def clear_all():
    """Erases every credential this tool has stored in the OS keyring."""
    delete_secret("mqtt_password")
    delete_secret("ssh_password")
