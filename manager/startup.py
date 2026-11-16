"""Per-user Windows sign-in entries for PoolFS configs; never store passwords."""

from __future__ import annotations

import hashlib
import os
import subprocess
import sys
from pathlib import Path

if os.name == "nt":
    import winreg


RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"


def entry_name(config_path: str) -> str:
    normalized = os.path.normcase(os.path.abspath(config_path))
    digest = hashlib.sha256(normalized.encode("utf-8")).hexdigest()[:16]
    return f"PoolFS-{digest}"


def startup_command(config_path: str) -> str:
    if getattr(sys, "frozen", False):
        arguments = [sys.executable]
    else:
        interpreter = Path(sys.executable)
        pythonw = interpreter.with_name("pythonw.exe")
        arguments = [str(pythonw if pythonw.is_file() else interpreter),
                     str(Path(__file__).with_name("app.py"))]
    arguments.extend(["--startup-mount", os.path.abspath(config_path)])
    command = subprocess.list2cmdline(arguments)
    if len(command) > 260:
        raise ValueError(
            "The manager and config paths are too long for Windows sign-in startup. "
            "Move the manager or config to a shorter path."
        )
    return command


def is_enabled(config_path: str) -> bool:
    if os.name != "nt":
        return False
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            value, kind = winreg.QueryValueEx(key, entry_name(config_path))
            return kind == winreg.REG_SZ and isinstance(value, str)
    except FileNotFoundError:
        return False


def set_enabled(config_path: str, enabled: bool) -> None:
    if os.name != "nt":
        raise RuntimeError("Mount at sign-in is currently available on Windows only.")
    name = entry_name(config_path)
    if enabled:
        command = startup_command(config_path)
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, RUN_KEY, 0,
                                winreg.KEY_SET_VALUE) as key:
            winreg.SetValueEx(key, name, 0, winreg.REG_SZ, command)
    else:
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0,
                                winreg.KEY_SET_VALUE) as key:
                winreg.DeleteValue(key, name)
        except FileNotFoundError:
            pass
