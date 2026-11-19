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

from PySide6.QtCore import Qt, QSettings, QTemporaryDir, QTimer  # noqa: E402
from PySide6.QtTest import QTest  # noqa: E402
from PySide6.QtWidgets import QApplication, QFileDialog, QMessageBox, QTableWidgetItem  # noqa: E402
from app import MIB, POOL_USAGE_ROLE, ManagerWindow, PoolWorkspace, validate_pool_name  # noqa: E402


def check_member_names(window: ManagerWindow) -> None:
    """Queued images use one ascending filename sequence across host volumes."""
    window.host_volumes = [
        {"diskNumber": index, "partitionNumber": 1, "filesystem": "NTFS",
         "mountPaths": [f"{letter}:\\"], "guidPath": f"volume-{letter}"}
        for index, letter in enumerate(("H", "T"), 1)
    ]
    window.volumes_table.blockSignals(True)
    window.volumes_table.setRowCount(2)
    for row in range(2):
        item = QTableWidgetItem("Use")
        item.setCheckState(Qt.CheckState.Unchecked)
        window.volumes_table.setItem(row, 0, item)
    window.volumes_table.blockSignals(False)

    def queue(*rows: int) -> None:
        for row in rows:
            window.volumes_table.item(row, 0).setCheckState(Qt.CheckState.Checked)
        assert window.queue_button.isEnabled()
        assert window.queue_button.text().startswith(f"Continue with {len(rows)} volume")
        window.queue_volumes()

    def names() -> list[str]:
        return [Path(str(member["path"])).name for member in window.members]

    with (patch.object(window, "_same_mount", return_value=True),
          patch("app.shutil.disk_usage", return_value=SimpleNamespace(free=10_000 * MIB)),
          patch.object(Path, "exists", autospec=True, return_value=False)):
        queue(0, 1)
        assert names() == ["member-1.img", "member-2.img"]
        window.members.clear()
        window._refresh_members()
        queue(0)
        queue(1)
        assert names() == ["member-1.img", "member-2.img"]

    window.members.clear()
    window._refresh_members()
    occupied = str(Path(r"H:\PoolFS\member-1.img")).casefold()
    with (patch.object(window, "_same_mount", return_value=True),
          patch("app.shutil.disk_usage", return_value=SimpleNamespace(free=10_000 * MIB)),
          patch.object(Path, "exists", autospec=True,
                       side_effect=lambda path: str(path).casefold() == occupied)):
        queue(0, 1)
        assert names() == ["member-2.img", "member-3.img"]
    with (patch.object(QFileDialog, "getSaveFileName",
                       return_value=(r"U:\PoolFS\member-2.img", "")),
          patch.object(window, "_error") as show_error):
        window.add_new_image_path()
        assert names() == ["member-2.img", "member-3.img"]
        show_error.assert_called_once()


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


def check_discovered_pool_flow(window: ManagerWindow) -> None:
    first = str(Path(__file__).resolve())
    second = str(Path(__file__).with_name("manager_startup_tests.py").resolve())
    def image(path: str, index: int) -> dict[str, object]:
        return {"path": path, "poolId": "pool-test-id", "memberIndex": index,
                "memberCount": 2, "bytes": 384 * MIB, "encrypted": False}

    window._on_images({"images": [image(first, 0)]})
    assert not window.image_action.isEnabled()
    window.images_table.selectRow(0)
    assert not window.image_action.isEnabled()
    assert "complete set" in window.image_selection.text()

    window._on_images({"images": [image(first, 0), image(second, 1),
                                  image(first + "-copy", 0)]})
    window.images_table.selectRow(0)
    assert not window.image_action.isEnabled()
    assert "Multiple copies" in window.image_selection.text()

    window._on_images({"images": [image(second, 1), image(first, 0)]})
    assert not window.image_action.isEnabled()
    window.images_table.selectRow(1)
    assert window.image_action.isEnabled()
    assert "All 2" in window.image_selection.text()
    window.backend.mounted_drives = ["P:"]  # type: ignore[attr-defined]
    with (patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.Yes),
          patch.object(window, "_drive_available", side_effect=lambda drive: drive == "Q:")):
        window.add_scanned_image()
        assert window.drive_combo.currentText() == "Q:"
        assert window.save_recovered_button.isEnabled()
        window.drive_combo.setCurrentText("P:")
        assert window.save_button.isEnabled(), "A recovered pool can save a new config on an occupied letter"
    assert [member["path"] for member in window.members] == [first, second]
    assert window.config_path is None
    assert not window.mount_button.isEnabled()
    assert window.pages.currentIndex() == 0
    assert "save a config" in window.pool_guidance.text()
    window.backend.mounted_drives = []  # type: ignore[attr-defined]


def main() -> None:
    application = QApplication([])
    temporary = QTemporaryDir()
    assert temporary.isValid()
    settings_path = str(Path(temporary.path()) / "manager.ini")
    settings = QSettings(settings_path, QSettings.Format.IniFormat)
    window = ManagerWindow(None, auto_refresh=False, settings=settings)
    window._drive_available = lambda _drive: True  # type: ignore[method-assign]
    assert window.pool_name.text() == "PoolFS"
    assert not window.health_refresh.isEnabled()
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

    check_member_names(window)

    saved: list[tuple[object, ...]] = []

    class FakeBackend:
        mounted_drives: list[str] = []
        stats_calls = 0
        health_calls = 0

        def save_config(self, *arguments: object) -> dict[str, str]:
            saved.append(arguments)
            return {"path": str(arguments[0])}

        def load_config(self, path: str) -> dict[str, object]:
            return {"name": "Travel Pool", "drive": "P:", "requireEncrypted": False,
                    "members": [str(Path(path).with_suffix(".img"))]}

        def mounts(self) -> dict[str, list[str]]:
            return {"mountedDrives": self.mounted_drives}

        def match_mount(self, drive: str, config: dict[str, object]) -> bool:
            return drive in self.mounted_drives and config["name"] == "Travel Pool"

        def health_config(self, path: str) -> dict[str, object]:
            self.health_calls += 1
            return {"name": "Travel Pool", "drive": "V:", "mounted": True,
                    "mountable": False, "members": [{"path": path, "status": "mounted"}]}

        def stats(self, drive: str) -> dict[str, int | str]:
            self.stats_calls += 1
            return {"drive": drive, "readBytes": self.stats_calls * MIB,
                    "writeBytes": self.stats_calls * 2 * MIB,
                    "readOperations": self.stats_calls,
                    "writeOperations": self.stats_calls * 2,
                    "uptimeMs": self.stats_calls * 1000,
                    "capacityBytes": 10 * MIB,
                    "availableBytes": 6 * MIB}

    fake_backend = FakeBackend()
    window.backend = fake_backend  # type: ignore[assignment]
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
    assert not any(button.isEnabled() for button in (
        window.save_button, window.mount_button, window.verify_button,
        window.grow_button, window.move_member_button, window.reclaim_button, window.unmount_button,
    ))
    other = str(Path(temporary.path()) / "another" / Path(destination).name)
    assert window.load_config_path(other)
    reopened = ManagerWindow(fake_backend, auto_refresh=False,
                             settings=QSettings(settings_path, QSettings.Format.IniFormat))
    assert reopened._recent_config_paths() == [other, destination]
    reopened._refresh_recent_configs()
    reopened.recent_configs_menu.actions()[1].trigger()
    assert reopened.config_path == destination
    assert window._recent_config_paths() == [destination, other]
    reopened._clear_recent_configs()
    assert window._recent_config_paths() == []

    image = str(Path(__file__))  # Existing file is sufficient for button state.
    window.config_path = image  # The button state checks that the saved file still exists.
    window.members = [{"path": image, "planned": False}]
    window.config = {**window.config, "members": [image]}
    window._refresh_members()
    with (patch("app.startup_is_enabled", return_value=False),
          patch("app.set_startup_enabled") as set_startup):
        window._refresh_startup_option()
        assert window.startup_check.isEnabled()
        assert not window.startup_check.isChecked()
        window.startup_check.setChecked(True)
        set_startup.assert_called_once_with(image, True)
        window.startup_check.setChecked(False)
        assert set_startup.call_args_list[-1].args == (image, False)
    assert all(button.isEnabled() for button in (
        window.save_button, window.mount_button, window.verify_button,
        window.grow_button, window.move_member_button,
    ))
    window.members_table.clearSelection()
    assert not window.move_member_button.isEnabled()
    window.members_table.selectRow(0)
    assert window.move_member_button.isEnabled()
    assert not window.reclaim_button.isEnabled()
    assert not window.unmount_button.isEnabled()

    fake_backend.mounted_drives = ["P:"]
    window._refresh_mount_state()
    assert not any(button.isEnabled() for button in (
        window.save_button, window.mount_button, window.verify_button,
        window.grow_button, window.move_member_button,
    ))
    assert window.reclaim_button.isEnabled()
    assert window.unmount_button.isEnabled()
    assert window.mount_indicator.text() == "P: · PoolFS mounted"
    window.pool_name.setText("Changed Pool")
    assert not window.reclaim_button.isEnabled()
    assert window.unmount_button.isEnabled()
    window.pool_name.setText("Travel Pool")
    fake_backend.mounted_drives = []
    window._refresh_mount_state()
    assert window.mount_button.isEnabled()
    assert window.move_member_button.isEnabled()
    assert not window.unmount_button.isEnabled()
    assert window.mount_indicator.text() == "P: · Unmounted"
    workspace = PoolWorkspace(fake_backend, auto_refresh=False, settings=settings)
    assert workspace.open_pool_config(destination)
    first = workspace.pool_stack.currentWidget()
    assert workspace.open_pool_config(other)
    second = workspace.pool_stack.currentWidget()
    assert first is not second and workspace.pool_list.count() == 2
    assert first.config_path == destination and second.config_path == other
    assert workspace.open_pool_config(destination)
    assert workspace.pool_stack.currentWidget() is first and workspace.pool_list.count() == 2
    second.config["name"] = "Another pool"
    second.pool_name.setText("Another pool")
    fake_backend.mounted_drives = ["P:"]
    workspace.refresh_mounts()
    assert first.unmount_button.isEnabled()
    assert first.mount_button.isHidden()
    assert not second.unmount_button.isEnabled()
    assert second.mount_indicator.text() == "P: · Different pool mounted"
    assert "Mounted" in workspace.pool_list.item(0).text()
    mounted_entry = workspace.pool_list.item(0)
    assert mounted_entry.data(POOL_USAGE_ROLE) == (6 * MIB, 10 * MIB)
    assert "40% used" in mounted_entry.toolTip()
    assert "6 MiB / 10 MiB free" in mounted_entry.toolTip()
    assert workspace.pool_list.item(1).data(POOL_USAGE_ROLE) == "Usage unavailable"
    fake_backend.mounted_drives = []
    member = Path(temporary.path()) / "present.img"
    member.touch()
    first.members[0]["path"] = str(member)
    with patch.object(first, "_drive_available", return_value=True):
        workspace.refresh_mounts()
    assert mounted_entry.data(POOL_USAGE_ROLE) == "Mount to see usage"
    workspace.pool_list.setCurrentRow(1)
    assert workspace.pool_stack.currentWidget() is second
    workspace.close_pool_button.click()
    assert workspace.pool_list.count() == 1 and first.config_path == destination
    workspace.close()
    fake_backend.mounted_drives = []

    check_discovered_pool_flow(window)
    check_image_size_controls(window)
    fake_backend.mounted_drives = ["P:"]
    fake_backend.stats_calls = 0
    with patch("app.time.monotonic", side_effect=[100.0, 101.0]):
        window._set_page("Stats")
        window._poll_stats()
    assert window.stats_drive.currentText() == "P:"
    assert window.stats_read_rate.text() == "1.0 MiB/s"
    assert window.stats_write_rate.text() == "2.0 MiB/s"
    assert window.stats_total_read.text() == "2 MiB"
    assert window.stats_usage.value() == 40
    assert window.benchmark_button.isEnabled()
    assert window.benchmark_size.value() == 32
    window.benchmark_size.setValue(8)
    benchmark_result = {"sizeMiB": 8, "readMiBPerSecond": 900.25, "writeMiBPerSecond": 134.5}
    with patch.object(fake_backend, "benchmark", create=True, return_value=benchmark_result) as run:
        window.benchmark_button.click()
        assert window._running_operation and not window.benchmark_button.isEnabled()
        deadline = time.monotonic() + 3
        while window._tasks and time.monotonic() < deadline:
            QTest.qWait(10)
        run.assert_called_once_with("P:", 8)
        assert window.benchmark_button.isEnabled()
        assert window.benchmark_status.text() == "P: · Read 900.25 MiB/s · Write 134.50 MiB/s · 8 MiB tested"
    fake_backend.mounted_drives = []
    window._refresh_mount_state()
    assert not window.benchmark_button.isEnabled() and window.benchmark_status.isHidden()
    window._show_health({"name": "Travel Pool", "mountable": False,
                         "driveStatus": "available", "lastVerifiedUtc": None,
                         "members": [{"path": image, "host": "C:",
                                      "hostFreeBytes": 8 * MIB, "status": "missing"}]})
    assert "Inspect member status below" in window.health_summary.text()
    assert window.health_table.rowCount() == 1
    assert window.health_table.item(0, 3).text() == "missing"
    window.config_path = image
    window._set_page("Health")
    for expected_calls in (1, 2):
        assert not window.health_refresh.isEnabled()
        assert window.health_refresh.text() == "Checking…"
        deadline = time.monotonic() + 3
        while not window.health_refresh.isEnabled() and time.monotonic() < deadline:
            QTest.qWait(10)
        assert window.health_refresh.isEnabled()
        assert fake_backend.health_calls == expected_calls
        assert "Mounted on V:" in window.health_summary.text()
        assert window.health_table.item(0, 3).text() == "mounted"
        assert window.health_checked.text().startswith("Last checked at ")
        if expected_calls == 1:
            window.health_refresh.click()
    fake_backend.mounted_drives = []
    window._set_page("Pool")
    events: list[bool] = []
    results: list[dict[str, bool]] = []

    def slow_work() -> dict[str, bool]:
        time.sleep(0.2)
        return {"done": True}

    def completed(result: dict[str, bool]) -> None:
        results.append(result)
        QTimer.singleShot(50, application.quit)

    assert window.create_progress.isHidden()
    window._run_operation("Creating PoolFS images…", slow_work, completed,
                          show_progress=True)
    assert not window.create_progress.isHidden()
    assert window.create_progress.minimum() == window.create_progress.maximum() == 0
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
    assert window.create_progress.isHidden()
    assert window.centralWidget().isEnabled()
    failures: list[str] = []

    def failing_work() -> None:
        raise RuntimeError("format failed")

    def failed(message: str) -> None:
        failures.append(message)
        QTimer.singleShot(50, application.quit)

    window._run_operation("Creating PoolFS images…", failing_work,
                          lambda _result: None, failed, show_progress=True)
    assert not window.create_progress.isHidden()
    QTimer.singleShot(3000, application.quit)
    application.exec()
    assert failures == ["format failed"]
    assert not window._running_operation
    assert window.create_progress.isHidden()
    assert window.centralWidget().isEnabled()
    print("PoolFS Qt manager task test passed")


if __name__ == "__main__":
    main()
