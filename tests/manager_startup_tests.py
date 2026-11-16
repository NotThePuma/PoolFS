"""Check sign-in entry identity and command construction without changing Windows."""

from __future__ import annotations

import os
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "manager"))

import startup  # noqa: E402


def main() -> None:
    path = str(Path(__file__).resolve())
    assert startup.entry_name(path) == startup.entry_name(path.upper())
    assert startup.entry_name(path) != startup.entry_name(path + "-other")
    command = startup.startup_command(path)
    assert "--startup-mount" in command
    assert path in command
    assert "password" not in command.casefold()
    assert len(command) <= 260
    try:
        startup.startup_command(str(Path("C:/") / ("long-name-" * 30) / "pool.json"))
    except ValueError as exc:
        assert "too long" in str(exc)
    else:
        raise AssertionError("An oversized Windows startup command was accepted")

    if os.name == "nt":
        key = MagicMock()
        key.__enter__.return_value = key
        with (patch.object(startup.winreg, "CreateKeyEx", return_value=key) as create,
              patch.object(startup.winreg, "SetValueEx") as write):
            startup.set_enabled(path, True)
            create.assert_called_once()
            write.assert_called_once_with(
                key, startup.entry_name(path), 0, startup.winreg.REG_SZ, command
            )
        with (patch.object(startup.winreg, "OpenKey", return_value=key),
              patch.object(startup.winreg, "QueryValueEx",
                           return_value=(command, startup.winreg.REG_SZ))):
            assert startup.is_enabled(path)
        with patch.object(startup.winreg, "OpenKey", side_effect=FileNotFoundError):
            assert not startup.is_enabled(path)
        with (patch.object(startup.winreg, "OpenKey", return_value=key),
              patch.object(startup.winreg, "DeleteValue") as delete):
            startup.set_enabled(path, False)
            delete.assert_called_once_with(key, startup.entry_name(path))

    print("PoolFS manager sign-in tests passed")


if __name__ == "__main__":
    main()
