"""Check that a native-style long task leaves the Qt event loop responsive."""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "manager"))

from PySide6.QtCore import QTimer  # noqa: E402
from PySide6.QtWidgets import QApplication, QFileDialog  # noqa: E402
from app import MIB, ManagerWindow, validate_pool_name  # noqa: E402


def check_image_size_controls(window: ManagerWindow) -> None:
    """Image sizes must stay within the selected host's usable free space."""
    first = {
        "path": r"D:\PoolFS\slider-test-1.img", "planned": True,
        "sizeMiB": 384, "hostRoot": "D:\\", "hostGuid": "slider-test-volume",
        "hostFilesystem": "NTFS",
    }
    second = {
        "path": r"D:\PoolFS\slider-test-2.img", "planned": True,
        "sizeMiB": 2000, "hostRoot": "D:\\", "hostGuid": "slider-test-volume",
        "hostFilesystem": "NTFS",
    }
    with patch("app.shutil.disk_usage", return_value=SimpleNamespace(free=10_000 * MIB)):
        window.members = [first, second]
        window._refresh_members()
        window.members_table.setCurrentCell(0, 0)
        assert window.size_slider.isEnabled()
        assert window.size_slider.minimum() == 384
        assert window.size_slider.maximum() == 8000  # reserves the other 2000 MiB

        window.size_slider.setValue(6000)
        assert first["sizeMiB"] == 6000
        assert window.size_input.text() == "6000"

        window.size_input.setText("5000")
        window.size_input.editingFinished.emit()
        assert first["sizeMiB"] == 5000
        assert window.size_slider.value() == 5000

        window.members_table.setCurrentCell(1, 0)
        assert window.size_slider.maximum() == 5000  # reserves the first member

        window.size_input.setText("3000")
        window.size_input.setModified(True)
        window.members_table.setCurrentCell(0, 0)
        assert second["sizeMiB"] == 3000  # changing rows commits the pending edit
        assert window.size_slider.maximum() == 7000

        free_mib = 10_000
        with patch("app.shutil.disk_usage", side_effect=lambda _root: SimpleNamespace(free=free_mib * MIB)):
            window._refresh_size_editor()
            free_mib = 7000
            window.size_slider.setValue(6500)
            assert first["sizeMiB"] == 5000
            assert window.size_slider.maximum() == 4000

        window.members = [{**first, "hostFilesystem": "FAT32", "sizeMiB": 384}]
        window._refresh_members()
        window.members_table.setCurrentCell(0, 0)
        assert window.size_slider.maximum() == 4095  # FAT32's per-file limit

    huge_free_mib = 2_147_484_148
    with patch("app.shutil.disk_usage", return_value=SimpleNamespace(free=huge_free_mib * MIB)):
        window.members = [{**first, "sizeMiB": 384}]
        window._refresh_members()
        assert window.size_slider.maximum() == 1_000_000
        window.size_input.setText(str(huge_free_mib))
        window.size_input.editingFinished.emit()
        assert window.members[0]["sizeMiB"] == huge_free_mib
        assert window.size_slider.value() == 1_000_000

    with patch("app.shutil.disk_usage", side_effect=OSError("volume unavailable")):
        window.members = [first]
        window._refresh_members()
        window.members_table.setCurrentCell(0, 0)
        assert not window.size_slider.isEnabled()
        assert not window.size_input.isEnabled()


def main() -> None:
    application = QApplication([])
    window = ManagerWindow(None, auto_refresh=False)
    assert window.pool_name.text() == "New Pool"
    assert not window.encrypt_check.isChecked()
    assert not window.require_check.isChecked()
    assert window.create_button.text() == "Create pool"
    window.encrypt_check.setChecked(True)
    assert window.require_check.isChecked()
    assert window.create_button.text() == "Create encrypted pool"
    window.new_pool()
    assert not window.encrypt_check.isChecked()
    assert not window.require_check.isChecked()
    assert validate_pool_name("Travel Pool") == "Travel Pool"
    for invalid in ("", " leading", "trailing ", "bad/name", "bad.", "x" * 33):
        try:
            validate_pool_name(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError(f"Invalid pool name was accepted: {invalid!r}")

    saved: list[tuple[object, ...]] = []

    class FakeBackend:
        def save_config(self, *arguments: object) -> dict[str, str]:
            saved.append(arguments)
            return {"path": str(arguments[0])}

    window.backend = FakeBackend()  # type: ignore[assignment]
    window.members = [{"path": str(Path(__file__).with_name("absent.img")),
                       "planned": False}]
    window.pool_name.setText("Travel Pool")
    window._refresh_members()
    destination = str(Path(__file__).with_name("named-pool.json"))
    with patch.object(QFileDialog, "getSaveFileName", return_value=(destination, "")):
        assert window.save_config()
    assert saved == [(destination, "Travel Pool", "P:",
                      [str(Path(__file__).with_name("absent.img"))], False)]
    assert window._current_config()["name"] == "Travel Pool"
    check_image_size_controls(window)
    events: list[bool] = []
    results: list[dict[str, bool]] = []

    def slow_work() -> dict[str, bool]:
        time.sleep(0.2)
        return {"done": True}

    def completed(result: dict[str, bool]) -> None:
        results.append(result)
        QTimer.singleShot(50, application.quit)

    window._run_operation("Checking…", slow_work, completed)
    QTimer.singleShot(
        40,
        lambda: events.append(window._running_operation and
                              not window.centralWidget().isEnabled()),
    )
    QTimer.singleShot(3000, application.quit)
    application.exec()
    assert events == [True], "the Qt event loop stalled during the operation"
    assert results == [{"done": True}], "the worker result was not delivered"
    assert not window._running_operation
    assert window.centralWidget().isEnabled()
    print("PoolFS Qt manager task test passed")


if __name__ == "__main__":
    main()
