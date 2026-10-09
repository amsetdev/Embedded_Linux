"""
transport.py
-------------
Abstract base class for all config-delivery transports (SSH, MQTT,
Serial). Concrete implementations live in their own modules; this file
defines the shared result type and the contract they must satisfy.
"""

from __future__ import annotations
from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Optional


@dataclass
class TransportResult:
    """Unified result returned by every transport operation."""
    ok: bool
    message: str
    ack_payload: Optional[dict] = None


class Transport(ABC):
    """Interface that every delivery backend must implement.

    Method signatures intentionally use ``**kwargs`` beyond the common
    positional arguments so that transport-specific extras (MQTT topics,
    serial login credentials, etc.) can pass through without polluting
    the base contract.
    """

    @abstractmethod
    def test_connection(self) -> TransportResult:
        """Verify that the transport can reach the target."""

    @abstractmethod
    def push_text(self, content: str, remote_path: str,
                  **kwargs) -> TransportResult:
        """Write arbitrary text content to the device."""

    @abstractmethod
    def push_combined_config(self, csv_content: str, json_content: str,
                             **kwargs) -> TransportResult:
        """Push both .csv and .json config files in one operation."""

    @abstractmethod
    def read_text(self, remote_path: str, **kwargs) -> TransportResult:
        """Read a file (or retained message) back from the device."""

    @abstractmethod
    def push_file(self, local_path: str, remote_path: str,
                  **kwargs) -> TransportResult:
        """Upload a local file to the device."""

    @abstractmethod
    def delete_remote(self, remote_path: str, **kwargs) -> TransportResult:
        """Remove a file (or clear a retained message) on the device."""
