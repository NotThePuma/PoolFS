"""Windows native bridge used by the Qt manager.

The window owns presentation and transient selections. The C++ bridge owns
PoolFS format, mount, verification, and partition safety checks.
"""

from __future__ import annotations

import ctypes
import json
import os
import sys
from pathlib import Path
from typing import Any, Callable


class BridgeError(RuntimeError):
    pass


class PartitionRequest(ctypes.Structure):
    _fields_ = [
        ("action", ctypes.c_uint32),
        ("disk_number", ctypes.c_uint32),
        ("expected_disk_bytes", ctypes.c_uint64),
        ("expected_disk_guid", ctypes.c_wchar_p),
        ("partition_number", ctypes.c_uint32),
        ("expected_partition_guid", ctypes.c_wchar_p),
        ("expected_partition_offset", ctypes.c_uint64),
        ("expected_partition_bytes", ctypes.c_uint64),
        ("size_mib", ctypes.c_uint64),
        ("drive_letter", ctypes.c_wchar),
        ("filesystem", ctypes.c_wchar_p),
        ("label", ctypes.c_wchar_p),
        ("disk_token", ctypes.c_wchar_p),
    ]


def bridge_path() -> Path:
    override = os.environ.get("POOLFS_BRIDGE_PATH")
    if override:
        candidate = Path(override).resolve()
        if candidate.is_file():
            return candidate
        raise BridgeError(f"POOLFS_BRIDGE_PATH does not exist: {candidate}")
    roots = (
        Path(sys.executable).resolve().parent,
        Path(__file__).resolve().parent,
        Path(__file__).resolve().parent.parent / "build",
        Path(__file__).resolve().parent.parent / "work" / "qt-build",
        Path(__file__).resolve().parent.parent / "site" / ".site-build",
    )
    for root in roots:
        candidate = root / "poolfs-gui-native.dll"
        if candidate.is_file():
            return candidate
    raise BridgeError(
        "poolfs-gui-native.dll is missing. Run the PoolFS build and keep the "
        "native bridge beside the manager."
    )


class NativeBackend:
    def __init__(self, path: Path | None = None) -> None:
        if os.name != "nt":
            raise BridgeError("The PoolFS storage backend currently requires Windows.")
        self.path = path or bridge_path()
        runtime = Path(r"C:\Program Files (x86)\WinFsp\bin")
        self._runtime_dll_dir = os.add_dll_directory(str(runtime)) if runtime.is_dir() else None
        self.dll = ctypes.CDLL(str(self.path))
        self.dll.poolfs_bridge_free.argtypes = [ctypes.c_void_p]
        self.dll.poolfs_bridge_free.restype = None
        self._bind("inventory", [])
        self._bind("scan_images", [ctypes.c_wchar_p, ctypes.c_int])
        self._bind("load_config", [ctypes.c_wchar_p])
        self._bind("save_named_config", [
            ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar,
            ctypes.POINTER(ctypes.c_wchar_p), ctypes.c_uint32, ctypes.c_int,
        ])
        self._bind("config_is_encrypted", [ctypes.c_wchar_p])
        self._bind("discover_config", [ctypes.c_wchar_p])
        self._bind("format_v6", [
            ctypes.POINTER(ctypes.c_wchar_p), ctypes.POINTER(ctypes.c_uint64),
            ctypes.c_uint32, ctypes.c_wchar_p, ctypes.c_uint32,
        ])
        self._bind("grow_v6", [
            ctypes.POINTER(ctypes.c_wchar_p), ctypes.POINTER(ctypes.c_wchar_p),
            ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint32,
            ctypes.c_wchar_p, ctypes.c_uint32,
        ])
        self._bind("mount_named_config", [
            ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar,
            ctypes.POINTER(ctypes.c_wchar_p), ctypes.c_uint32, ctypes.c_int,
            ctypes.c_wchar_p, ctypes.c_uint32,
        ])
        self._bind("verify_config", [
            ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32,
        ])
        self._bind("unmount", [ctypes.c_wchar])
        self._bind("mounts", [])
        self._bind("partition", [
            ctypes.POINTER(PartitionRequest), ctypes.c_int,
        ])

    def _bind(self, name: str, argument_types: list[Any]) -> None:
        function = getattr(self.dll, f"poolfs_bridge_{name}")
        function.argtypes = [*argument_types, ctypes.POINTER(ctypes.c_void_p)]
        function.restype = ctypes.c_int

    def _call(self, name: str, *arguments: Any) -> dict[str, Any]:
        response = ctypes.c_void_p()
        function: Callable[..., int] = getattr(self.dll, f"poolfs_bridge_{name}")
        status = function(*arguments, ctypes.byref(response))
        if not response.value:
            raise BridgeError(f"Native bridge returned no {name} response.")
        try:
            payload = ctypes.string_at(response).decode("utf-8")
        finally:
            if response.value:
                self.dll.poolfs_bridge_free(response)
        try:
            data = json.loads(payload)
        except json.JSONDecodeError as exc:
            raise BridgeError(f"Native bridge returned invalid JSON for {name}.") from exc
        if status != 0:
            raise BridgeError(str(data.get("error", f"{name} failed.")))
        if not isinstance(data, dict):
            raise BridgeError(f"Native bridge returned an invalid {name} result.")
        return data

    def inventory(self) -> dict[str, Any]:
        return self._call("inventory")

    def scan_images(self, root: str | None = None, recursive: bool = False) -> dict[str, Any]:
        return self._call("scan_images", root, int(recursive))

    def load_config(self, path: str) -> dict[str, Any]:
        return self._call("load_config", path)

    def save_config(
        self, path: str, name: str, drive: str,
        members: list[str], require_encrypted: bool
    ) -> dict[str, Any]:
        paths = (ctypes.c_wchar_p * len(members))(*members)
        return self._call(
            "save_named_config", path, name, drive[0], paths, len(members), int(require_encrypted)
        )

    def discover_config(self, path: str) -> dict[str, Any]:
        return self._call("discover_config", path)

    def config_is_encrypted(self, path: str) -> dict[str, Any]:
        return self._call("config_is_encrypted", path)

    def _password_call(
        self, name: str, arguments: tuple[Any, ...], password: str | None
    ) -> dict[str, Any]:
        if password is None:
            return self._call(name, *arguments, None, 0)
        if not password:
            raise BridgeError("PoolFS password cannot be empty.")
        secret = ctypes.create_unicode_buffer(password)
        try:
            return self._call(name, *arguments, secret, len(password))
        finally:
            ctypes.memset(ctypes.addressof(secret), 0, ctypes.sizeof(secret))

    def format_v6(
        self, members: list[tuple[str, int]], password: str | None
    ) -> dict[str, Any]:
        paths = (ctypes.c_wchar_p * len(members))(
            *(path for path, _ in members)
        )
        sizes = (ctypes.c_uint64 * len(members))(
            *(size_bytes for _, size_bytes in members)
        )
        return self._password_call(
            "format_v6", (paths, sizes, len(members)), password
        )

    def grow_v6(
        self, members: list[tuple[str, str, int]], password: str | None
    ) -> dict[str, Any]:
        sources = (ctypes.c_wchar_p * len(members))(*(source for source, _, _ in members))
        destinations = (ctypes.c_wchar_p * len(members))(
            *(destination for _, destination, _ in members)
        )
        sizes = (ctypes.c_uint64 * len(members))(
            *(size_bytes for _, _, size_bytes in members)
        )
        return self._password_call(
            "grow_v6", (sources, destinations, sizes, len(members)), password
        )

    def mount_config(
        self, path: str, expected: dict[str, Any], password: str | None
    ) -> dict[str, Any]:
        members = list(expected["members"])
        paths = (ctypes.c_wchar_p * len(members))(*members)
        drive = str(expected["drive"])[0]
        return self._password_call(
            "mount_named_config",
            (
                path, str(expected.get("name") or "PoolFS"), drive, paths, len(members),
                int(bool(expected["requireEncrypted"])),
            ),
            password,
        )

    def verify_config(
        self, path: str, password: str | None
    ) -> dict[str, Any]:
        return self._password_call("verify_config", (path,), password)

    def mounts(self) -> dict[str, Any]:
        return self._call("mounts")

    def unmount(self, drive: str) -> dict[str, Any]:
        return self._call("unmount", drive[0])

    def partition(self, request: PartitionRequest, apply: bool = False) -> dict[str, Any]:
        return self._call("partition", ctypes.byref(request), int(apply))
