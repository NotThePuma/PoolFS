"""Smoke the public native GUI ABI without changing host partitions or mounts."""

from __future__ import annotations

import argparse
import ctypes
import json
import shutil
import tempfile
from pathlib import Path


def bind(dll: ctypes.CDLL, name: str, arguments: list[object]):
    function = getattr(dll, name)
    function.argtypes = arguments + [ctypes.POINTER(ctypes.c_void_p)]
    function.restype = ctypes.c_int
    return function


def call(dll: ctypes.CDLL, function, *arguments):
    pointer = ctypes.c_void_p()
    status = function(*arguments, ctypes.byref(pointer))
    try:
        if not pointer.value:
            raise AssertionError("native bridge returned no JSON response")
        result = json.loads(ctypes.string_at(pointer).decode("utf-8"))
    finally:
        if pointer.value:
            dll.poolfs_bridge_free(pointer)
    return status, result


def require_ok(result):
    status, payload = result
    if status != 0:
        raise AssertionError(f"native bridge failed: {payload}")
    return payload


def free_drive() -> str:
    kernel32 = ctypes.windll.kernel32
    kernel32.GetDriveTypeW.argtypes = [ctypes.c_wchar_p]
    kernel32.GetDriveTypeW.restype = ctypes.c_uint32
    for code in range(ord("Z"), ord("M") - 1, -1):
        letter = chr(code)
        if kernel32.GetDriveTypeW(f"{letter}:\\") == 1:
            return letter
    raise AssertionError("no unused drive letter for bridge integration test")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--integration", action="store_true")
    parser.add_argument("--work-root", type=Path,
                        help="Directory for disposable member images")
    options = parser.parse_args()

    dll = ctypes.CDLL(str(options.dll.resolve()))
    dll.poolfs_bridge_free.argtypes = [ctypes.c_void_p]
    dll.poolfs_bridge_free.restype = None
    output = ctypes.POINTER(ctypes.c_void_p)
    paths = ctypes.POINTER(ctypes.c_wchar_p)
    sizes = ctypes.POINTER(ctypes.c_uint64)
    inventory = bind(dll, "poolfs_bridge_inventory", [])
    scan = bind(dll, "poolfs_bridge_scan_images", [ctypes.c_wchar_p, ctypes.c_int])
    save = bind(dll, "poolfs_bridge_save_named_config", [
        ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar,
        paths, ctypes.c_uint32, ctypes.c_int])
    old_save = bind(dll, "poolfs_bridge_save_config", [
        ctypes.c_wchar_p, ctypes.c_wchar, paths, ctypes.c_uint32, ctypes.c_int])
    load = bind(dll, "poolfs_bridge_load_config", [ctypes.c_wchar_p])
    encrypted = bind(dll, "poolfs_bridge_config_is_encrypted", [ctypes.c_wchar_p])
    format_v6 = bind(dll, "poolfs_bridge_format_v6", [
        paths, sizes, ctypes.c_uint32, ctypes.c_wchar_p, ctypes.c_uint32])
    grow_v6 = bind(dll, "poolfs_bridge_grow_v6", [
        paths, paths, sizes, ctypes.c_uint32, ctypes.c_wchar_p, ctypes.c_uint32])
    verify = bind(dll, "poolfs_bridge_verify_config", [
        ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32])
    mount = bind(dll, "poolfs_bridge_mount_named_config", [
        ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar,
        paths, ctypes.c_uint32,
        ctypes.c_int, ctypes.c_wchar_p, ctypes.c_uint32])
    old_mount = bind(dll, "poolfs_bridge_mount_config", [
        ctypes.c_wchar_p, ctypes.c_wchar, paths, ctypes.c_uint32,
        ctypes.c_int, ctypes.c_wchar_p, ctypes.c_uint32])
    mounts = bind(dll, "poolfs_bridge_mounts", [])
    unmount = bind(dll, "poolfs_bridge_unmount", [ctypes.c_wchar])

    listed = require_ok(call(dll, inventory))
    assert isinstance(listed["disks"], list)
    assert isinstance(listed["volumes"], list)
    assert isinstance(listed["partitionElevated"], bool)

    workspace = Path(__file__).resolve().parents[1]
    work = (options.work_root or workspace / "work").resolve()
    work.mkdir(parents=True, exist_ok=True)
    fixture = Path(tempfile.mkdtemp(prefix="gui-bridge-", dir=work)).resolve()
    if not fixture.is_relative_to(work):
        raise AssertionError("bridge fixture escaped the work directory")
    try:
        scanned = require_ok(call(dll, scan, str(fixture), 0))
        assert scanned["images"] == []
        assert scanned["errors"] == []

        image = fixture / "member.img"
        config = fixture / "pool.json"
        members = (ctypes.c_wchar_p * 1)(str(image))
        drive = free_drive()
        saved = require_ok(call(dll, save, str(config), "Travel Pool", drive, members, 1, 1))
        assert Path(saved["path"]).resolve() == config
        loaded = require_ok(call(dll, load, str(config)))
        assert loaded["name"] == "Travel Pool"
        assert loaded["drive"] == f"{drive}:"
        assert loaded["requireEncrypted"] is True
        assert Path(loaded["members"][0]).resolve() == image
        require_ok(call(dll, old_save, str(config), drive, members, 1, 1))
        assert require_ok(call(dll, load, str(config)))["name"] == "Travel Pool"
        legacy = fixture / "legacy.json"
        require_ok(call(dll, old_save, str(legacy), drive, members, 1, 0))
        old_loaded = require_ok(call(dll, load, str(legacy)))
        assert old_loaded["name"] == "PoolFS"
        assert old_loaded["requireEncrypted"] is False

        if options.integration:
            password = ctypes.create_unicode_buffer("BridgeTestPassword!2026")
            try:
                values = (ctypes.c_uint64 * 1)(384 * 1024 * 1024)
                require_ok(call(dll, format_v6, members, values, 1,
                                password, len(password.value)))
                assert require_ok(call(dll, encrypted, str(config)))["encrypted"]
                checked = require_ok(call(dll, verify, str(config),
                                          password, len(password.value)))
                assert checked["issues"] == 0
                assert checked["incompleteTail"] is False
                # The displayed config snapshot must be rechecked before mount.
                status, failure = call(dll, mount, str(config), "Travel Pool", "A", members,
                                       1, 1, password, len(password.value))
                assert status == 1
                assert "changed since it was shown" in failure["error"]
                status, failure = call(dll, mount, str(config), "Other Pool", drive, members,
                                       1, 1, password, len(password.value))
                assert status == 1
                assert "changed since it was shown" in failure["error"]
                status, failure = call(dll, old_mount, str(config), "A", members,
                                       1, 1, password, len(password.value))
                assert status == 1
                assert "changed since it was shown" in failure["error"]
                active = False
                try:
                    require_ok(call(dll, mount, str(config), "Travel Pool", drive, members,
                                    1, 1, password, len(password.value)))
                    active = True
                    assert f"{drive}:" in require_ok(call(dll, mounts))["mountedDrives"]
                finally:
                    if active:
                        require_ok(call(dll, unmount, drive))
                assert f"{drive}:" not in require_ok(call(dll, mounts))["mountedDrives"]
                grown_image = fixture / "member-grown.img"
                grown_config = fixture / "pool-grown.json"
                grown_members = (ctypes.c_wchar_p * 1)(str(grown_image))
                grown_sizes = (ctypes.c_uint64 * 1)(512 * 1024 * 1024)
                require_ok(call(dll, grow_v6, members, grown_members, grown_sizes,
                                1, password, len(password.value)))
                assert image.stat().st_size == 384 * 1024 * 1024
                require_ok(call(dll, save, str(grown_config), "Travel Pool", drive,
                                grown_members, 1, 1))
                assert require_ok(call(dll, verify, str(grown_config),
                                       password, len(password.value)))["issues"] == 0
                old_active = False
                try:
                    require_ok(call(dll, old_mount, str(config), drive, members,
                                    1, 1, password, len(password.value)))
                    old_active = True
                finally:
                    if old_active:
                        require_ok(call(dll, unmount, drive))
            finally:
                for index in range(len(password)):
                    password[index] = "\0"
            plain_image = fixture / "plain.img"
            plain_config = fixture / "plain.json"
            plain_members = (ctypes.c_wchar_p * 1)(str(plain_image))
            plain_sizes = (ctypes.c_uint64 * 1)(384 * 1024 * 1024)
            require_ok(call(dll, save, str(plain_config), "Plain Pool", drive,
                            plain_members, 1, 0))
            require_ok(call(dll, format_v6, plain_members, plain_sizes, 1, None, 0))
            assert not require_ok(call(dll, encrypted, str(plain_config)))["encrypted"]
            assert require_ok(call(dll, verify, str(plain_config), None, 0))["issues"] == 0
            plain_active = False
            try:
                require_ok(call(dll, mount, str(plain_config), "Plain Pool", drive,
                                plain_members, 1, 0, None, 0))
                plain_active = True
                assert f"{drive}:" in require_ok(call(dll, mounts))["mountedDrives"]
            finally:
                if plain_active:
                    require_ok(call(dll, unmount, drive))
            plain_grown = fixture / "plain-grown.img"
            plain_grown_paths = (ctypes.c_wchar_p * 1)(str(plain_grown))
            require_ok(call(dll, grow_v6, plain_members, plain_grown_paths,
                            (ctypes.c_uint64 * 1)(512 * 1024 * 1024), 1, None, 0))
            assert plain_image.stat().st_size == 384 * 1024 * 1024
    finally:
        if not fixture.resolve().is_relative_to(work):
            raise AssertionError("unsafe bridge fixture cleanup path")
        shutil.rmtree(fixture)

    print("PoolFS native GUI bridge tests passed")


if __name__ == "__main__":
    main()
