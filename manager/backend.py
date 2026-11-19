"""Windows native bridge used by the Qt manager.

The window owns presentation and transient selections. The C++ bridge owns
PoolFS format, mount, verification, and partition safety checks.
"""

from __future__ import annotations

import ctypes
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable


class BridgeError(RuntimeError):
    pass


@dataclass(frozen=True)
class JournalReport:
    status: str
    details: str


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
        self._bind("reclaim", [ctypes.c_wchar])
        self._bind("mounts", [])
        if hasattr(self.dll, "poolfs_bridge_match_mount"):
            self._bind("match_mount", [
                ctypes.c_wchar, ctypes.POINTER(ctypes.c_wchar_p), ctypes.c_uint32, ctypes.c_int,
            ])
        self._bind("stats", [ctypes.c_wchar])
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

    def _run_controller(self, *arguments: str, timeout: float | None = None) -> subprocess.CompletedProcess[str]:
        controller = self.path.parent / "poolfsctl.exe"
        return subprocess.run(
            [str(controller), *arguments], capture_output=True, text=True,
            encoding="utf-8", errors="replace", creationflags=subprocess.CREATE_NO_WINDOW,
            check=False, timeout=timeout,
        )

    def move_member(self, config: str, source: str, destination: str) -> str:
        try:
            result = self._run_controller("move-member", config, source, destination)
        except FileNotFoundError as exc:
            raise BridgeError("Moving a member requires poolfsctl.exe beside the native library.") from exc
        if result.returncode != 0:
            raise BridgeError(result.stderr.strip() or "Could not move the member image.")
        return result.stderr.strip()

    def health_config(self, path: str) -> dict[str, Any]:
        try:
            result = self._run_controller("health-config", path, "--json", timeout=15)
        except subprocess.TimeoutExpired as exc:
            raise BridgeError("Health check timed out. Check that the host drives respond and try again.") from exc
        except FileNotFoundError as exc:
            raise BridgeError("Health needs poolfsctl.exe beside the PoolFS native library.") from exc
        if result.returncode not in (0, 2):
            raise BridgeError(result.stderr.strip() or "Could not inspect pool health.")
        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError as exc:
            raise BridgeError("Pool health returned invalid data.") from exc

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

    def check_journal(
        self, path: str, config: dict[str, Any], password: str | None, *, repair: bool = False
    ) -> JournalReport:
        if self.load_config(path) != config:
            raise BridgeError("The saved config changed. Reopen it before checking the journal.")

        checker = self.path.parent / "poolfs-check.exe"
        arguments = [str(checker)]
        if repair:
            arguments.append("--repair-final-tail")
        arguments.extend(("--password-stdin", *config["members"]))
        secret = bytearray(password or "", "utf-8")
        try:
            if secret:
                secret[:0] = len(secret).to_bytes(4, "little")
            result = subprocess.run(
                arguments, input=secret, capture_output=True,
                creationflags=subprocess.CREATE_NO_WINDOW, check=False,
            )
        except FileNotFoundError as exc:
            raise BridgeError("Journal checks need poolfs-check.exe beside the PoolFS native library.") from exc
        finally:
            secret[:] = bytes(len(secret))

        details = "\n".join(
            output.decode("utf-8", errors="replace").strip()
            for output in (result.stdout, result.stderr) if output
        )
        status = next((line.removeprefix("status: ") for line in reversed(details.splitlines())
                       if line.startswith("status: ")), "")
        exit_codes = {"CLEAN": 0, "INTERRUPTED_FINAL_COMMIT": 2, "PENDING_REPAIR": 3,
                      "CORRUPT": 3, "ERROR": 1, "REPAIRED": 0}
        if exit_codes.get(status) != result.returncode or (status == "REPAIRED" and not repair):
            raise BridgeError(details or f"PoolFS journal checker exited with code {result.returncode}.")
        return JournalReport(status, details)

    def test_write(self, config: dict[str, Any], folder: str) -> None:
        drive = config["drive"]
        # PoolFS has no reparse points, and realpath cannot resolve its WinFsp volume.
        directory = Path(os.path.abspath(folder))
        if directory.drive.upper() != drive or not directory.is_dir():
            raise BridgeError(f"Choose a folder on the mounted PoolFS drive {drive}.")
        if self.match_mount(drive, config) is not True:
            raise BridgeError(f"Could not confirm that this pool is mounted on {drive}.")

        payload = b"PoolFS write test\n"
        probe = None
        failure = None
        try:
            with tempfile.NamedTemporaryFile(
                dir=directory, prefix="poolfs-write-test-", suffix=".tmp", delete=False
            ) as stream:
                probe = Path(stream.name)
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
                stream.seek(0)
                if stream.read() != payload:
                    raise BridgeError("The test file contents did not match the written data.")
        except (OSError, BridgeError) as exc:
            failure = exc
        if probe is not None:
            try:
                probe.unlink()
            except OSError as exc:
                cleanup_error = f"Could not remove the test file {probe}. {exc}"
                raise BridgeError(f"{failure}\n{cleanup_error}" if failure else cleanup_error) from exc
        if failure:
            raise failure

    def mounts(self) -> dict[str, Any]:
        return self._call("mounts")

    def match_mount(self, drive: str, config: dict[str, Any]) -> bool | None:
        if not hasattr(self.dll, "poolfs_bridge_match_mount"):
            return None
        members = config["members"]
        paths = (ctypes.c_wchar_p * len(members))(*members)
        return self._call("match_mount", drive[0], paths, len(members),
                          int(config["requireEncrypted"]))["matches"]

    def stats(self, drive: str) -> dict[str, Any]:
        return self._call("stats", drive[0])

    def benchmark(self, drive: str, size_mib: int) -> dict[str, Any]:
        try:
            result = self._run_controller("benchmark", drive, "--size-mib", str(size_mib), "--json")
        except FileNotFoundError as exc:
            raise BridgeError("The benchmark needs poolfsctl.exe beside the PoolFS native library.") from exc
        if result.returncode != 0:
            raise BridgeError(result.stderr.strip().removeprefix("poolfsctl: ") or "Benchmark failed.")
        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError as exc:
            raise BridgeError("The benchmark returned invalid data.") from exc

    def unmount(self, drive: str) -> dict[str, Any]:
        return self._call("unmount", drive[0])

    def reclaim(self, drive: str) -> dict[str, Any]:
        return self._call("reclaim", drive[0])

    def partition(self, request: PartitionRequest, apply: bool = False) -> dict[str, Any]:
        return self._call("partition", ctypes.byref(request), int(apply))
