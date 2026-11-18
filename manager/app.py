"""PoolFS desktop manager. Qt owns presentation; the C++ bridge owns storage."""

from __future__ import annotations

import argparse
import ctypes
import os
import shutil
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

from PySide6.QtCore import Qt, QPoint, QRect, QSettings, QSize, QThread, Signal, QTimer, QRegularExpression
from PySide6.QtGui import QColor, QFont, QKeySequence, QPainter, QPainterPath, QPen
from PySide6.QtGui import QRegularExpressionValidator
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QDialog, QDialogButtonBox,
    QFileDialog, QFormLayout, QFrame, QGridLayout, QHBoxLayout, QHeaderView,
    QLabel, QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QMenu, QMessageBox,
    QProgressBar, QPushButton, QScrollArea, QStyledItemDelegate,
    QSlider, QSpinBox, QStackedWidget, QTableWidget, QTableWidgetItem, QVBoxLayout,
    QWidget, QAbstractItemView,
)

from backend import BridgeError, NativeBackend, PartitionRequest
from startup import is_enabled as startup_is_enabled, set_enabled as set_startup_enabled


MIB = 1024 * 1024
MIN_IMAGE_MIB = 384
MAX_IMAGE_MIB = ((1 << 63) - 1) // MIB
MAX_SLIDER_STEPS = 1_000_000
NEW_POOL_NAME = "PoolFS"
RECENT_CONFIGS_KEY = "recentConfigPaths"
RECENT_CONFIGS_LIMIT = 8
POOL_USAGE_ROLE = Qt.ItemDataRole.UserRole + 1
STYLE = """
QWidget { color: #edf3ff; font-family: "Segoe UI"; font-size: 12px; }
QMainWindow, QWidget#root { background: #101721; }
QLabel { background: transparent; }
QFrame#sidebar { background: #131c28; border-right: 1px solid #2a3749; }
QFrame#card { background: #172230; border: 1px solid #2c394b; border-radius: 11px; }
QFrame#poolLead { background: #1a2939; border: 1px solid #385369; border-radius: 11px; }
QFrame#accentCard { background: #15343d; border: 1px solid #35716f; border-radius: 11px; }
QFrame#metricCard { background: #1b2a3b; border: 1px solid #304155; border-radius: 8px; }
QFrame#statusBar { background: #131c28; border-top: 1px solid #2a3749; }
QLabel#eyebrow { color: #77e5c1; font-size: 10px; font-weight: 700; letter-spacing: 1px; }
QLabel#title { color: #f0f6ff; font-size: 29px; font-weight: 700; }
QLabel#section { color: #edf3ff; font-size: 18px; font-weight: 650; }
QLabel#muted { color: #9db0c9; font-size: 12px; }
QLabel#micro { color: #8198b5; font-size: 10px; font-weight: 650; }
QLabel#metricValue { color: #f4f8ff; font-size: 25px; font-weight: 700; }
QPushButton { border: 1px solid #334760; border-radius: 9px; padding: 9px 14px;
              background: #24354d; color: #e9f2ff; font-weight: 600; }
QPushButton:hover { background: #304663; border-color: #6b91a8; }
QPushButton:pressed { background: #1c2d42; }
QPushButton:disabled { color: #6f8198; background: #182638; border-color: #27384d; }
QPushButton#primary { background: #77e5c1; border-color: #77e5c1; color: #092018; }
QPushButton#primary:hover { background: #9cf1d4; border-color: #9cf1d4; }
QPushButton#primary:disabled { background: #285247; border-color: #285247; color: #6d9b8b; }
QPushButton#nav { background: transparent; border: 1px solid transparent; border-radius: 6px;
                  text-align: left; color: #b6c7db; padding: 7px 10px; font-size: 13px; }
QPushButton#nav:hover { background: #1a2a3d; color: #edf3ff; }
QPushButton#nav:checked { background: #1d3941; color: #77e5c1; }
QPushButton#nav:disabled { color: #687a90; }
QPushButton#view { background: transparent; border: 1px solid transparent;
                   padding: 7px 14px; color: #b6c7db; }
QPushButton#view:hover { color: #edf3ff; background: #253749; }
QPushButton#view:checked { color: #a5f3d8; background: #213e43; border-color: #34585d; }
QPushButton#view:focus, QPushButton#nav:focus { border-color: #77e5c1; }
QListWidget#openPools { background: transparent; border: none; }
QListWidget#openPools::item { margin: 2px 0; padding: 7px 9px; border-radius: 7px;
                             border: 1px solid #2d3c4e; background: #182432; }
QListWidget#openPools::item:hover { background: #223649; }
QListWidget#openPools::item:selected { background: #244049; border-color: #4d8885; color: #f0f6ff; }
QMenu#recentConfigs { background: #17263a; border: 1px solid #39506c; padding: 5px; }
QMenu#recentConfigs::item { padding: 7px 14px; }
QMenu#recentConfigs::item:selected { background: #214c51; }
QMenu#recentConfigs::item:disabled { color: #9db0c9; }
QTableWidget { background: #172230; alternate-background-color: #1b2939;
               border: none; gridline-color: #2a3950; selection-background-color: #214c51;
               selection-color: #ffffff; outline: none; }
QTableWidget::item { padding: 6px 9px; border-bottom: 1px solid #223248; }
QTableWidget::item:selected { background: #214c51; color: #ffffff; }
QTableWidget::indicator { width: 17px; height: 17px; }
QTableWidget::indicator:unchecked { background: #1b2d43; border: 2px solid #8cb7cc;
                                    border-radius: 4px; }
QTableWidget::indicator:unchecked:hover { border-color: #77e5c1; }
QTableWidget::indicator:checked { background: #77e5c1; border: 2px solid #77e5c1;
                                  border-radius: 4px; }
QHeaderView::section { background: #1c2b40; color: #a9bfd8; border: none;
                       border-bottom: 1px solid #33455e; padding: 8px 9px; font-weight: 600; }
QLineEdit, QComboBox, QSpinBox { background: #17263a; border: 1px solid #39506c;
                                 border-radius: 7px; padding: 7px; color: #edf3ff; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: #77e5c1; }
QSlider::groove:horizontal { height: 6px; border-radius: 3px; background: #30445e; }
QSlider::sub-page:horizontal { border-radius: 3px; background: #77e5c1; }
QSlider::handle:horizontal { width: 17px; margin: -6px 0; border-radius: 8px;
                             background: #e5fff4; border: 2px solid #77e5c1; }
QSlider:disabled::handle:horizontal { background: #62768b; border-color: #62768b; }
QComboBox QAbstractItemView { background: #17263a; selection-background-color: #214c51; }
QCheckBox { spacing: 8px; color: #d7e5f6; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #71859b;
                       border-radius: 4px; background: #101721; }
QCheckBox::indicator:checked { background: #77e5c1; border-color: #77e5c1; }
QCheckBox::indicator:disabled { border-color: #405267; }
QCheckBox::indicator:checked:disabled { background: #467566; }
QScrollArea { border: none; background: transparent; }
QScrollArea QWidget#qt_scrollarea_viewport, QWidget#page { background: #101721; }
QScrollBar:vertical { width: 10px; background: #111d2d; }
QScrollBar::handle:vertical { background: #344c65; border-radius: 5px; min-height: 30px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QProgressBar { background: #17263a; border: 1px solid #39506c;
               border-radius: 5px; height: 9px; }
QProgressBar::chunk { background: #77e5c1; border-radius: 4px; }
QMessageBox, QDialog { background: #121d2e; }
"""


def label(text: str, kind: str | None = None, wrap: bool = False) -> QLabel:
    item = QLabel(text)
    if kind:
        item.setObjectName(kind)
    item.setWordWrap(wrap)
    return item


def button(text: str, callback: Callable[[], None], primary: bool = False) -> QPushButton:
    item = QPushButton(text)
    if primary:
        item.setObjectName("primary")
    item.clicked.connect(callback)
    return item


def recent_menu(trigger: QPushButton) -> QMenu:
    menu = QMenu(trigger)
    menu.setObjectName("recentConfigs")
    menu.setToolTipsVisible(True)
    trigger.setMenu(menu)
    return menu


def card(accent: bool = False) -> tuple[QFrame, QVBoxLayout]:
    frame = QFrame()
    frame.setObjectName("accentCard" if accent else "card")
    box = QVBoxLayout(frame)
    box.setContentsMargins(20, 18, 20, 18)
    box.setSpacing(11)
    return frame, box


def table(headers: list[str], multi: bool = False) -> QTableWidget:
    widget = QTableWidget(0, len(headers))
    widget.setHorizontalHeaderLabels(headers)
    widget.verticalHeader().hide()
    widget.setShowGrid(False)
    widget.setAlternatingRowColors(True)
    widget.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
    widget.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
    widget.setSelectionMode(
        QAbstractItemView.SelectionMode.ExtendedSelection if multi
        else QAbstractItemView.SelectionMode.SingleSelection
    )
    widget.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.ResizeToContents)
    widget.horizontalHeader().setStretchLastSection(True)
    widget.setMinimumHeight(135)
    return widget


def human_size(value: Any) -> str:
    if value is None:
        return "Unknown"
    number = float(value)
    if number >= 1024**4:
        return f"{number / 1024**4:.1f} TiB"
    if number >= 1024**3:
        return f"{number / 1024**3:.1f} GiB"
    if number >= MIB:
        return f"{number / MIB:.0f} MiB"
    return f"{number:,.0f} B"


def pool_usage_labels(available: int, capacity: int) -> tuple[str, str]:
    percent = (capacity - available) * 100 // capacity
    used = "<1% used" if percent == 0 and available < capacity else f"{percent}% used"
    return used, f"{human_size(available)} / {human_size(capacity)} free"


class PoolEntryDelegate(QStyledItemDelegate):
    def paint(self, painter: QPainter, option: Any, index: Any) -> None:
        super().paint(painter, option, index)
        usage = index.data(POOL_USAGE_ROLE)
        if usage is None:
            return
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        font = QFont(option.font)
        font.setPixelSize(11)
        painter.setFont(font)
        painter.setPen(QColor("#b6c7db"))
        footer = option.rect.adjusted(11, option.rect.height() - 25, -11, -8)
        if isinstance(usage, str):
            painter.drawText(footer, Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, usage)
        else:
            available, capacity = usage
            percent, free = pool_usage_labels(available, capacity)
            painter.drawText(footer, Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, percent)
            room = max(0, footer.width() - painter.fontMetrics().horizontalAdvance(percent) - 8)
            free = painter.fontMetrics().elidedText(free, Qt.TextElideMode.ElideRight, room)
            painter.drawText(footer, Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, free)
            track = QRect(footer.left(), footer.top() - 9, footer.width(), 5)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor("#364b60"))
            painter.drawRoundedRect(track, 2, 2)
            if available < capacity:
                track.setWidth(max(1, track.width() * (capacity - available) // capacity))
                painter.setBrush(QColor("#77e5c1"))
                painter.drawRoundedRect(track, 2, 2)
        painter.restore()


def human_rate(bytes_per_second: float) -> str:
    if bytes_per_second >= MIB:
        return f"{bytes_per_second / MIB:.1f} MiB/s"
    if bytes_per_second >= 1024:
        return f"{bytes_per_second / 1024:.1f} KiB/s"
    return f"{bytes_per_second:.0f} B/s"


class SpeedGraph(QWidget):
    """A rolling view of filesystem throughput over the last 60 samples."""

    def __init__(self) -> None:
        super().__init__()
        self.setMinimumHeight(180)
        self.samples: list[tuple[float, float]] = []

    def add_sample(self, read_rate: float, write_rate: float) -> None:
        self.samples.append((read_rate, write_rate))
        self.samples = self.samples[-60:]
        self.update()

    def clear(self) -> None:
        self.samples.clear()
        self.update()

    def paintEvent(self, event: Any) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), QColor("#162438"))
        left, top = 18, 16
        width = max(1, self.width() - 36)
        height = max(1, self.height() - 32)
        painter.setPen(QPen(QColor("#30445e"), 1))
        for fraction in (0.0, 0.5, 1.0):
            y = top + int(height * fraction)
            painter.drawLine(left, y, left + width, y)
        peak = max((max(pair) for pair in self.samples), default=0.0)
        ceiling = max(1024.0, peak * 1.1)
        for column, color in ((0, "#77e5c1"), (1, "#73aaff")):
            path = QPainterPath()
            for index, sample in enumerate(self.samples):
                x = left + width * index / 59
                y = top + height * (1.0 - min(sample[column] / ceiling, 1.0))
                if index == 0:
                    path.moveTo(x, y)
                else:
                    path.lineTo(x, y)
            painter.setPen(QPen(QColor(color), 2))
            painter.drawPath(path)


def validate_pool_name(name: str) -> str:
    try:
        units = len(name.encode("utf-16-le")) // 2
    except UnicodeEncodeError as exc:
        raise ValueError("Pool name contains an invalid Unicode character.") from exc
    if not 1 <= units <= 32:
        raise ValueError("Pool name must be 1 to 32 characters long.")
    if name != name.strip(" ") or name.endswith("."):
        raise ValueError("Pool name cannot start or end with a space or end with a period.")
    if any(ord(char) < 32 or ord(char) == 127 or char in '<>:/\\|?*"' for char in name):
        raise ValueError("Pool name contains a character Windows cannot use in a drive label.")
    return name


class Task(QThread):
    result = Signal(object)
    failed = Signal(str)

    def __init__(self, action: Callable[[], Any], parent: QWidget) -> None:
        super().__init__(parent)
        self.action = action

    def run(self) -> None:
        try:
            self.result.emit(self.action())
        except Exception as exc:
            self.failed.emit(str(exc))


class PasswordDialog(QDialog):
    def __init__(self, parent: QWidget, title: str, confirm: bool = False) -> None:
        super().__init__(parent)
        self.setWindowTitle(title)
        self.setMinimumWidth(390)
        box = QVBoxLayout(self)
        box.setContentsMargins(22, 22, 22, 18)
        box.setSpacing(14)
        box.addWidget(label("PoolFS password", "section"))
        box.addWidget(label("The password is used for this operation and is never saved in the config.", "muted", True))
        self.first = QLineEdit()
        self.first.setEchoMode(QLineEdit.EchoMode.Password)
        self.first.setPlaceholderText("Enter password")
        box.addWidget(self.first)
        self.second: QLineEdit | None = None
        if confirm:
            self.second = QLineEdit()
            self.second.setEchoMode(QLineEdit.EchoMode.Password)
            self.second.setPlaceholderText("Confirm password")
            box.addWidget(self.second)
        controls = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        controls.accepted.connect(self._accept)
        controls.rejected.connect(self.reject)
        box.addWidget(controls)

    def _accept(self) -> None:
        if not self.first.text():
            QMessageBox.warning(self, "Password required", "Enter the PoolFS password.")
            return
        if self.second is not None and self.first.text() != self.second.text():
            QMessageBox.warning(self, "Passwords differ", "The two passwords do not match.")
            self.second.clear()
            return
        self.accept()

    def take_password(self) -> str:
        value = self.first.text()
        self.first.clear()
        if self.second is not None:
            self.second.clear()
        return value


class ManagerWindow(QMainWindow):
    def __init__(self, backend: NativeBackend | None = None, auto_refresh: bool = True,
                 settings: QSettings | None = None,
                 workspace: PoolWorkspace | None = None) -> None:
        super().__init__()
        self.backend = backend
        self.workspace = workspace
        self.settings = settings if settings is not None else QSettings(
            QSettings.Format.NativeFormat, QSettings.Scope.UserScope, "PoolFS", "Manager", self
        )
        self.settings.setFallbacksEnabled(False)
        self.inventory: dict[str, Any] = {"disks": [], "volumes": []}
        self._inventory_loading = False
        self._inventory_loaded = False
        self.host_volumes: list[dict[str, Any]] = []
        self.config: dict[str, Any] | None = None
        self.config_path: str | None = None
        self.members: list[dict[str, Any]] = []
        self.images: list[dict[str, Any]] = []
        self._selected_image_group: list[dict[str, Any]] = []
        self._images_scanned = False
        self._tasks: set[Task] = set()
        self._running_operation = False
        self._health_check: tuple[str, object] | None = None
        self._mounted_drives: set[str] | None = None
        self._stats_previous: dict[str, tuple[float, int, int, int]] = {}
        self.setWindowTitle("PoolFS Manager")
        if self.workspace is None:
            self.resize(1050, 750)
            self.setMinimumSize(800, 620)
        self._build_shell()
        self._size_validation_timer = QTimer(self)
        self._size_validation_timer.setSingleShot(True)
        self._size_validation_timer.timeout.connect(self._refresh_create_button)
        self._mount_state_timer = QTimer(self)
        self._mount_state_timer.timeout.connect(self._refresh_mount_state)
        if workspace is None:
            self._mount_state_timer.start(2500)
        self._stats_timer = QTimer(self)
        self._stats_timer.timeout.connect(self._poll_stats)
        self._stats_timer.start(1000)
        self._set_page("Pool")
        self._refresh_members()
        if auto_refresh:
            QTimer.singleShot(0, self.refresh_inventory)

    def _build_shell(self) -> None:
        root = QWidget()
        root.setObjectName("root")
        self.setCentralWidget(root)
        shell = QVBoxLayout(root)
        shell.setContentsMargins(0, 0, 0, 0)
        shell.setSpacing(0)
        main = QWidget()
        main_box = QVBoxLayout(main)
        main_box.setContentsMargins(24, 20, 20, 16)
        main_box.setSpacing(14)
        navigation = QHBoxLayout()
        navigation.setSpacing(5)
        self.nav: dict[str, QPushButton] = {}
        views = [("Pool", "Overview"), ("Health", "Health"), ("Stats", "Activity")]
        if self.workspace is None:
            views.extend((("Storage", "Storage"), ("Images", "Find images")))
        for name, caption in views:
            item = QPushButton(caption)
            item.setObjectName("view")
            item.setCheckable(True)
            item.clicked.connect(lambda checked=False, page=name: self._set_page(page))
            navigation.addWidget(item)
            self.nav[name] = item
        navigation.addStretch()
        if self.workspace is None:
            navigation.addWidget(button("About PoolFS", self._show_about))
        main_box.addLayout(navigation)
        self.page_header = QWidget()
        title_stack = QVBoxLayout(self.page_header)
        title_stack.setContentsMargins(0, 0, 0, 0)
        title_stack.setSpacing(4)
        self.page_title = label("Pool", "title")
        title_stack.addWidget(self.page_title)
        self.page_subtitle = label("", "muted", True)
        title_stack.addWidget(self.page_subtitle)
        main_box.addWidget(self.page_header)
        self.pages = QStackedWidget()
        main_box.addWidget(self.pages, 1)
        self.pages.addWidget(self._build_pool_page())
        self.pages.addWidget(self._build_storage_page())
        self.pages.addWidget(self._build_images_page())
        self.pages.addWidget(self._build_stats_page())
        self.pages.addWidget(self._build_health_page())
        shell.addWidget(main, 1)

        status_frame = QFrame()
        status_frame.setObjectName("statusBar")
        status_frame.setFixedHeight(40)
        status_box = QHBoxLayout(status_frame)
        status_box.setContentsMargins(23, 0, 23, 0)
        self.status = label("Loading local storage…", "muted")
        status_box.addWidget(self.status, 1)
        self.create_progress = QProgressBar()
        self.create_progress.setObjectName("createProgress")
        self.create_progress.setAccessibleName("Creating PoolFS member images")
        self.create_progress.setTextVisible(False)
        self.create_progress.setFixedWidth(220)
        self.create_progress.setRange(0, 0)
        self.create_progress.hide()
        status_box.addWidget(self.create_progress)
        shell.addWidget(status_frame)

    def _scroll_page(self, content: QWidget) -> QScrollArea:
        area = QScrollArea()
        area.setWidgetResizable(True)
        area.setWidget(content)
        return area

    def _show_about(self) -> None:
        dialog = QDialog(self)
        dialog.setWindowTitle("About PoolFS")
        dialog.setMinimumWidth(420)
        box = QVBoxLayout(dialog)
        box.setContentsMargins(24, 22, 24, 20)
        box.setSpacing(12)
        box.addWidget(label("PoolFS Manager", "section"))
        box.addWidget(label("v6 · image-backed pools", "muted"))
        box.addWidget(label("All member images are required to mount a pool.", "muted", True))
        box.addSpacing(10)
        box.addWidget(label("WinFsp attribution", "micro"))
        winfsp_notice = label(
            'WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos<br>'
            '<a href="https://github.com/winfsp/winfsp">WinFsp source</a>',
            "muted", True,
        )
        winfsp_notice.setOpenExternalLinks(True)
        box.addWidget(winfsp_notice)
        close_button = button("Close", dialog.accept)
        box.addWidget(close_button, alignment=Qt.AlignmentFlag.AlignRight)
        dialog.exec()

    def _build_pool_page(self) -> QScrollArea:
        content = QWidget()
        content.setObjectName("page")
        layout = QVBoxLayout(content)
        layout.setContentsMargins(0, 0, 10, 5)
        layout.setSpacing(16)
        current, current_box = card()
        current.setObjectName("poolLead")
        top = QHBoxLayout()
        top.setSpacing(18)
        detail = QVBoxLayout()
        detail.setSpacing(7)
        self.config_name = label("Set up a pool", "title", True)
        detail.addWidget(self.config_name)
        self.mount_indicator = label("", wrap=True)
        self.mount_indicator.setToolTip("Mount status of the selected drive letter")
        self.mount_indicator.hide()
        detail.addWidget(self.mount_indicator, alignment=Qt.AlignmentFlag.AlignLeft)
        self.config_location = label("", "muted", True)
        detail.addWidget(self.config_location)
        self.pool_guidance = label("Choose host volumes to create a pool, or open a saved config.", "muted", True)
        detail.addWidget(self.pool_guidance)
        top.addLayout(detail, 1)
        primary_actions = QVBoxLayout()
        primary_actions.setSpacing(7)
        self.choose_volumes_button = button(
            "Choose host volumes", lambda: self._set_page("Storage"), True
        )
        primary_actions.addWidget(self.choose_volumes_button)
        self.save_recovered_button = button("Save config to mount", self.save_config, True)
        primary_actions.addWidget(self.save_recovered_button)
        if self.workspace is None:
            primary_actions.addWidget(button("New pool", self.new_pool))
            primary_actions.addWidget(button("Open configs…", self.open_config))
            recent = QPushButton("Recent configs")
            self.recent_configs_menu = recent_menu(recent)
            self.recent_configs_menu.aboutToShow.connect(self._refresh_recent_configs)
            primary_actions.addWidget(recent)
        top.addLayout(primary_actions)
        current_box.addLayout(top)
        layout.addWidget(current)

        columns = QHBoxLayout()
        columns.setSpacing(16)
        members_card, members_box = card()
        member_heading = QHBoxLayout()
        member_heading.addWidget(label("Member images", "section"), 1)
        self.member_count = label("0 of 16 member images", "muted")
        member_heading.addWidget(self.member_count)
        members_box.addLayout(member_heading)
        self.member_empty = label(
            "Choose host volumes for new images, or add images you already have.",
            "muted", True,
        )
        members_box.addWidget(self.member_empty)
        self.members_table = table(["Image path", "Size", "State"])
        self.members_table.setFixedHeight(220)
        self.members_table.horizontalHeader().setStretchLastSection(False)
        self.members_table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.members_table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        self.members_table.horizontalHeader().setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
        self.members_table.itemSelectionChanged.connect(self._refresh_size_editor)
        self.members_table.itemSelectionChanged.connect(self._refresh_member_controls)
        members_box.addWidget(self.members_table)
        size_frame = QFrame()
        self.size_frame = size_frame
        size_frame.setObjectName("metricCard")
        size_box = QVBoxLayout(size_frame)
        size_box.setContentsMargins(16, 14, 16, 14)
        size_box.setSpacing(9)
        size_heading = QHBoxLayout()
        size_heading.addWidget(label("NEW IMAGE SIZE", "eyebrow"))
        self.size_selected = label("Select a new image above", "muted")
        self.size_selected.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        self.size_selected.setToolTip("Select a new image path to set its size")
        size_heading.addWidget(self.size_selected, 1)
        size_box.addLayout(size_heading)
        size_controls = QHBoxLayout()
        self.size_slider = QSlider(Qt.Orientation.Horizontal)
        self.size_slider.setObjectName("imageSizeSlider")
        self.size_slider.setMinimumWidth(140)
        self.size_slider.valueChanged.connect(self._size_slider_changed)
        size_controls.addWidget(self.size_slider, 1)
        self.size_input = QLineEdit()
        self.size_input.setObjectName("imageSizeMiB")
        self.size_input.setFixedWidth(112)
        self.size_input.setAlignment(Qt.AlignmentFlag.AlignRight)
        self.size_input.setValidator(QRegularExpressionValidator(QRegularExpression(r"[0-9]{0,13}")))
        self.size_input.editingFinished.connect(self._size_input_finished)
        self.size_input.setAccessibleName("Exact new image size in MiB")
        size_controls.addWidget(self.size_input)
        size_controls.addWidget(label("MiB", "muted"))
        size_box.addLayout(size_controls)
        self.size_maximum = label("Select a new member image to set its size.", "muted", True)
        size_box.addWidget(self.size_maximum)
        members_box.addWidget(size_frame)
        controls = QHBoxLayout()
        self.add_existing_button = button("Add existing…", self.add_existing_image)
        self.new_image_button = button("New image path…", self.add_new_image_path)
        self.remove_member_button = button("Remove", self.remove_selected_member)
        for item in (self.add_existing_button, self.new_image_button, self.remove_member_button):
            controls.addWidget(item)
        controls.addStretch()
        members_box.addLayout(controls)
        columns.addWidget(members_card, 1)
        side = QVBoxLayout()
        side.setSpacing(16)
        details, details_box = card()
        details.setFixedWidth(302)
        details_box.addWidget(label("Pool settings", "section"))
        details_box.addWidget(label("POOL NAME", "micro"))
        self.pool_name = QLineEdit(NEW_POOL_NAME)
        self.pool_name.setMaxLength(32)
        self.pool_name.setPlaceholderText("Name this pool")
        self.pool_name.textChanged.connect(lambda _text: self._refresh_pool_identity())
        self.pool_name.textChanged.connect(self._refresh_create_button)
        self.pool_name.textChanged.connect(self._refresh_pool_actions)
        details_box.addWidget(self.pool_name)
        details_box.addWidget(label("MOUNT DRIVE", "micro"))
        self.drive_combo = QComboBox()
        self.drive_combo.addItems([f"{chr(code)}:" for code in range(ord("D"), ord("Z") + 1)])
        self.drive_combo.setCurrentText("P:")
        self.drive_combo.currentTextChanged.connect(self._refresh_pool_actions)
        details_box.addWidget(self.drive_combo)
        self.encrypt_check = QCheckBox("Encrypt new pool")
        self.encrypt_check.setChecked(False)
        details_box.addWidget(self.encrypt_check)
        self.encryption_note = label(
            "Unencrypted member images can be read without a password.", "muted", True
        )
        self.encryption_note.setStyleSheet("color:#efbd79;")
        details_box.addWidget(self.encryption_note)
        self.require_check = QCheckBox("Require encryption when mounting")
        self.require_check.setChecked(False)
        self.require_check.toggled.connect(self._refresh_pool_actions)
        details_box.addWidget(self.require_check)
        self.startup_check = QCheckBox("Mount when I sign in to Windows")
        self.startup_check.setToolTip(
            "Starts this pool at your Windows sign-in. Encrypted pools ask for the password."
        )
        self.startup_check.toggled.connect(self._startup_changed)
        details_box.addWidget(self.startup_check)
        side.addWidget(details)
        columns.addLayout(side)
        columns.setAlignment(members_card, Qt.AlignmentFlag.AlignTop)
        actions, actions_box = card()
        self.actions_card = actions
        self.create_button = button("Create pool", self.create_pool, True)
        self.grow_button = button("Grow images…", self.grow_images)
        self.encrypt_check.toggled.connect(self._encryption_changed)
        self.save_button = button("Save config…", self.save_config)
        self.mount_button = button("Mount", self.mount_pool, True)
        self.verify_button = button("Verify data", self.verify_pool)
        self.reclaim_button = button("Reclaim space", self.reclaim_space)
        self.unmount_button = button("Unmount", self.unmount_pool)
        self.mount_button.setMinimumWidth(106)
        self.unmount_button.setMinimumWidth(106)
        self.verify_button.setToolTip("Unmount the pool before checking file data.")
        self.grow_button.setToolTip("Unmount the pool before expanding its member images.")
        self.reclaim_button.setToolTip("Release unused checkpoint space on the mounted pool.")
        for item in (self.create_button, self.mount_button, self.unmount_button):
            primary_actions.addWidget(item)
        details_box.addWidget(self.save_button)
        row = QHBoxLayout()
        row.addWidget(label("Maintenance", "section"), 1)
        for item in (self.verify_button, self.grow_button, self.reclaim_button):
            row.addWidget(item)
        actions_box.addLayout(row)
        layout.addLayout(columns)
        layout.addWidget(actions)
        layout.addStretch(1)
        return self._scroll_page(content)

    def _metric(self, title: str) -> tuple[QFrame, QLabel]:
        frame = QFrame()
        frame.setObjectName("metricCard")
        box = QVBoxLayout(frame)
        box.setContentsMargins(18, 14, 18, 14)
        box.addWidget(label(title, "micro"))
        value = label("—", "metricValue")
        box.addWidget(value)
        return frame, value

    def _build_storage_page(self) -> QScrollArea:
        content = QWidget()
        content.setObjectName("page")
        layout = QVBoxLayout(content)
        layout.setContentsMargins(0, 0, 10, 5)
        layout.setSpacing(16)
        toolbar = QHBoxLayout()
        toolbar.addStretch()
        toolbar.addWidget(button("Refresh storage", self.refresh_inventory))
        toolbar.addWidget(button("Disk tools  ↓", self._scroll_to_partition_tools))
        layout.addLayout(toolbar)
        metrics = QHBoxLayout()
        metrics.setSpacing(12)
        for title, attribute in (
            ("PHYSICAL DISKS", "disk_count"),
            ("PARTITIONS", "partition_count"),
            ("FORMATTED HOST VOLUMES", "volume_count"),
        ):
            frame, value = self._metric(title)
            setattr(self, attribute, value)
            metrics.addWidget(frame, 1)
        layout.addLayout(metrics)
        lists = QHBoxLayout()
        lists.setSpacing(16)
        disk_card, disk_box = card()
        disk_box.addWidget(label("Disks", "section"))
        disk_box.addWidget(label("Select one to inspect its partitions.", "muted"))
        self.disks_table = table(["Disk", "Size", "Layout", "Details"])
        self.disks_table.setMinimumHeight(220)
        self.disks_table.itemSelectionChanged.connect(self._selected_disk_changed)
        disk_box.addWidget(self.disks_table)
        lists.addWidget(disk_card, 1)
        part_card, part_box = card()
        part_box.addWidget(label("Partitions on selected disk", "section"))
        self.partition_context = label("Choose a disk on the left.", "muted")
        part_box.addWidget(self.partition_context)
        self.partitions_table = table(["Part", "Drive", "File system", "Size", "Type / label"])
        self.partitions_table.setMinimumHeight(220)
        self.partitions_table.itemSelectionChanged.connect(self._update_partition_buttons)
        part_box.addWidget(self.partitions_table)
        lists.addWidget(part_card, 1)
        layout.addLayout(lists)
        partition_actions, partition_box = card()
        self.partition_actions = partition_actions
        partition_heading = QHBoxLayout()
        partition_heading.addWidget(label("Partition tools", "section"), 1)
        self.admin_button = button("Run as admin", self.restart_as_admin)
        partition_heading.addWidget(self.admin_button)
        partition_box.addLayout(partition_heading)
        self.partition_notice = label(
            "Changes require Administrator and all PoolFS drives unmounted. "
            "The target is previewed and confirmed before any write.", "muted", True
        )
        partition_box.addWidget(self.partition_notice)
        buttons = QHBoxLayout()
        self.partition_buttons: dict[int, QPushButton] = {}
        for action, title in (
            (0, "Initialize GPT"), (1, "Create + format"), (2, "Quick format"),
            (3, "Remove"), (4, "Change letter"),
        ):
            item = button(title, lambda action=action: self.partition_action(action))
            item.setEnabled(False)
            self.partition_buttons[action] = item
            buttons.addWidget(item)
        buttons.addStretch()
        partition_box.addLayout(buttons)
        layout.addWidget(partition_actions)
        volumes_card, volumes_box = card()
        header = QHBoxLayout()
        description = QVBoxLayout()
        description.addWidget(label("Host volumes", "section"))
        description.addWidget(label(
            "Select 1–16 formatted volumes. One member image will be created on each after review.",
            "muted", True
        ))
        header.addLayout(description, 1)
        self.queue_button = button("Select host volumes", self.queue_volumes, True)
        self.queue_button.setEnabled(False)
        header.addWidget(self.queue_button)
        volumes_box.addLayout(header)
        self.volumes_table = table(["Use", "Mount path", "File system", "Free", "Disk / part", "Label"])
        self.volumes_table.setFixedHeight(120)
        self.volumes_table.setSelectionMode(QAbstractItemView.SelectionMode.NoSelection)
        self.volumes_table.itemChanged.connect(self._volume_selection_changed)
        self.volumes_table.cellClicked.connect(self._volume_cell_clicked)
        volumes_box.addWidget(self.volumes_table)
        self.volume_selection = label("No host volumes selected.", "muted")
        volumes_box.addWidget(self.volume_selection)
        layout.insertWidget(2, volumes_card)
        self.storage_scroll = self._scroll_page(content)
        return self.storage_scroll

    def _scroll_to_partition_tools(self) -> None:
        QTimer.singleShot(
            0, lambda: self.storage_scroll.verticalScrollBar().setValue(
                self.partition_actions.mapTo(self.storage_scroll.widget(), QPoint(0, 0)).y()
            ),
        )

    def _build_images_page(self) -> QScrollArea:
        content = QWidget()
        content.setObjectName("page")
        layout = QVBoxLayout(content)
        layout.setContentsMargins(0, 0, 10, 5)
        layout.setSpacing(16)
        image_card, image_box = card()
        row = QHBoxLayout()
        self.image_count = label("No scan yet", "muted")
        row.addWidget(self.image_count, 1)
        row.addWidget(button("Scan again", self.scan_images))
        image_box.addLayout(row)
        self.images_table = table(["Image path", "Size", "Pool", "Member", "Encrypted"])
        self.images_table.setFixedHeight(120)
        self.images_table.horizontalHeader().setStretchLastSection(False)
        self.images_table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.images_table.itemSelectionChanged.connect(self._refresh_image_action)
        image_box.addWidget(self.images_table)
        self.image_selection = label("Select an image to see whether its complete pool is here.", "muted", True)
        self.image_action = button("Select an image", self.add_scanned_image, True)
        self.image_action.setEnabled(False)
        selection = QHBoxLayout()
        selection.addWidget(self.image_selection, 1)
        selection.addWidget(self.image_action)
        image_box.addLayout(selection)
        layout.addWidget(image_card)
        layout.addStretch()
        return self._scroll_page(content)

    def _build_stats_page(self) -> QScrollArea:
        content = QWidget()
        content.setObjectName("page")
        layout = QVBoxLayout(content)
        layout.setContentsMargins(0, 0, 10, 5)
        layout.setSpacing(16)
        heading, heading_box = card()
        row = QHBoxLayout()
        detail = QVBoxLayout()
        detail.addWidget(label(
            "Rates update each second.",
            "muted", True,
        ))
        row.addLayout(detail, 1)
        row.addWidget(label("MOUNTED DRIVE", "micro"))
        self.stats_drive = QComboBox()
        self.stats_drive.setMinimumWidth(95)
        self.stats_drive.currentTextChanged.connect(self._stats_drive_changed)
        row.addWidget(self.stats_drive)
        row.addWidget(button("Refresh", self._refresh_stats))
        heading_box.addLayout(row)
        self.stats_status = label("Mount a pool to see live statistics.", "muted", True)
        heading_box.addWidget(self.stats_status)
        layout.addWidget(heading)

        rates = QHBoxLayout()
        read_card, self.stats_read_rate = self._metric("READ SPEED")
        write_card, self.stats_write_rate = self._metric("WRITE SPEED")
        rates.addWidget(read_card, 1)
        rates.addWidget(write_card, 1)
        layout.addLayout(rates)

        graph_card, graph_box = card()
        graph_box.addWidget(label("Recent speed · last 60 seconds", "section"))
        graph_box.addWidget(label(
            '<span style="color:#77e5c1">● Read</span> &nbsp;&nbsp; '
            '<span style="color:#73aaff">● Write</span>', "muted"
        ))
        self.stats_graph = SpeedGraph()
        graph_box.addWidget(self.stats_graph)
        layout.addWidget(graph_card)

        totals = QHBoxLayout()
        for title, attribute in (
            ("TOTAL READ", "stats_total_read"),
            ("TOTAL WRITTEN", "stats_total_write"),
            ("READ OPERATIONS", "stats_read_ops"),
            ("WRITE OPERATIONS", "stats_write_ops"),
        ):
            frame, value = self._metric(title)
            setattr(self, attribute, value)
            totals.addWidget(frame, 1)
        layout.addLayout(totals)

        capacity_card, capacity_box = card()
        capacity_box.addWidget(label("Pool space", "section"))
        self.stats_capacity = label("Mount a pool to see its available space.", "muted")
        capacity_box.addWidget(self.stats_capacity)
        self.stats_usage = QProgressBar()
        self.stats_usage.setRange(0, 100)
        capacity_box.addWidget(self.stats_usage)
        layout.addWidget(capacity_card)
        layout.addStretch(1)
        self._clear_stats()
        return self._scroll_page(content)

    def _build_health_page(self) -> QScrollArea:
        content = QWidget()
        content.setObjectName("page")
        layout = QVBoxLayout(content)
        layout.setContentsMargins(0, 0, 10, 5)
        layout.setSpacing(16)
        overview, box = card()
        headline = QHBoxLayout()
        self.health_config_label = label("", "muted", True)
        self.health_config_label.hide()
        self.health_summary = label("Open a pool config to check its health.", "section", True)
        headline.addWidget(self.health_summary, 1)
        self.health_refresh = button("Refresh health", self.refresh_health)
        self.health_refresh.setEnabled(False)
        headline.addWidget(self.health_refresh)
        box.addLayout(headline)
        box.addWidget(self.health_config_label)
        self.health_checked = label("No health check run", "muted")
        box.addWidget(self.health_checked)
        self.health_verified = label("Last successful verification: not recorded", "muted", True)
        box.addWidget(self.health_verified)
        layout.addWidget(overview)
        members_card, members_box = card()
        members_box.addWidget(label("Required members", "section"))
        self.health_table = table(["Member image", "Host", "Host free", "Status"])
        self.health_table.horizontalHeader().setStretchLastSection(False)
        self.health_table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        members_box.addWidget(self.health_table)
        layout.addWidget(members_card)
        layout.addStretch()
        return self._scroll_page(content)

    def _set_page(self, name: str) -> None:
        pages = {"Pool": 0, "Storage": 1, "Images": 2, "Stats": 3, "Health": 4}
        self.pages.setCurrentIndex(pages[name])
        page = self.pages.widget(pages[name])
        if isinstance(page, QScrollArea):
            QTimer.singleShot(0, lambda: page.verticalScrollBar().setValue(0))
        self.page_header.setVisible(name != "Pool")
        self.page_title.setText({
            "Stats": "Pool activity", "Images": "Find member images", "Storage": "Disks & volumes",
        }.get(name, name))
        subtitles = {
            "Pool": "Create a pool or open an existing config.",
            "Storage": "Choose volumes for new images or inspect physical disks.",
            "Images": "Scan the PoolFS folders on your host volumes for existing member images.",
            "Stats": "Read and write rates, capacity, and totals for a mounted pool.",
            "Health": "Check member images and see when the pool was last verified.",
        }
        self.page_subtitle.setText(subtitles[name])
        for title, control in self.nav.items():
            control.setChecked(title == name)
        if self.workspace:
            self.workspace.update_navigation(self, name)
        if name == "Images" and not self._images_scanned:
            self.scan_images()
        if name == "Storage" and self.workspace and not self._inventory_loaded:
            self.refresh_inventory()
        if name == "Stats":
            drive = self.drive_combo.currentText()
            if self.workspace and self.stats_drive.findText(drive) >= 0:
                self.stats_drive.setCurrentText(drive)
            self._refresh_stats()
        if name == "Health":
            self.refresh_health()

    def refresh_health(self) -> None:
        path = self.config_path
        backend = self.backend
        if not path or backend is None:
            self._health_check = None
            self.health_refresh.setEnabled(False)
            self.health_refresh.setText("Refresh health")
            self.health_config_label.hide()
            self.health_summary.setText("Open a pool config to check its health.")
            self.health_table.setRowCount(0)
            self.health_checked.setText("No health check run")
            self.health_verified.setText("Last successful verification: not recorded")
            return
        if self._health_check and self._health_check[0] == path:
            return
        request = (path, object())
        self._health_check = request
        self.health_config_label.setText(f"Saved config: {path}")
        self.health_config_label.show()
        self.health_summary.setText("Checking pool members…")
        self.health_checked.clear()
        self.health_refresh.setText("Checking…")
        self.health_refresh.setEnabled(False)
        self.health_table.setRowCount(0)
        self.health_verified.setText("Last successful verification: not recorded")

        def inspect() -> dict[str, Any]:
            try:
                return backend.health_config(path)
            except Exception as exc:
                return {"error": str(exc)}

        def completed(report: dict[str, Any]) -> None:
            if self._health_check is not request:
                return
            self._health_check = None
            if self.config_path != path:
                self.refresh_health()
                return
            self.health_refresh.setText("Refresh health")
            self.health_refresh.setEnabled(True)
            self._show_health(report)

        self._run(inspect, completed)

    def _show_health(self, report: dict[str, Any]) -> None:
        if "error" in report:
            self.health_summary.setText(str(report["error"]))
            self.health_table.setRowCount(0)
            self.health_checked.setText(f"Check failed at {datetime.now():%H:%M:%S}")
            return
        members = report.get("members", [])
        drive = report.get("drive", "")
        if report.get("mounted"):
            summary = f"Mounted on {drive} · all {len(members)} configured members in use"
        elif report.get("mountable"):
            summary = f"Unmounted · {len(members)} member headers checked · ready to mount on {drive}"
        else:
            summary = str(report.get("reason") or "Inspect member status below")
        self.health_summary.setText(f"{report.get('name', 'PoolFS')}: {summary}")
        self.health_table.setRowCount(len(members))
        self.health_table.setFixedHeight(min(420, max(120, 43 + len(members) * 39)))
        for row, member in enumerate(members):
            path = str(member.get("path", ""))
            self._put(self.health_table, row, 0, path, row)
            self.health_table.item(row, 0).setToolTip(path)
            self._put(self.health_table, row, 1, str(member.get("host", "")))
            self._put(self.health_table, row, 2, human_size(member.get("hostFreeBytes")))
            self._put(self.health_table, row, 3, str(member.get("status", "unknown")))
        self.health_verified.setText(
            "Last successful verification: " + str(report.get("lastVerifiedUtc") or "not recorded")
        )
        self.health_checked.setText(f"Last checked at {datetime.now():%H:%M:%S}")

    def _message(self, text: str) -> None:
        self.status.setText(text)

    def _refresh_pool_identity(self) -> None:
        if not hasattr(self, "config_name"):
            return
        if self.config_path or self.members:
            self.config_name.setText(self.pool_name.text() or "Unnamed pool")
            location = self.config_path or (
                    "Unsaved new pool · set sizes and create images."
                    if any(member.get("planned") for member in self.members)
                    else "Existing member images · save a config to mount."
            )
            self.config_location.setText(location)
            self.config_location.setToolTip(self.config_path or "")
            self.config_location.show()
        else:
            self.config_name.setText("Set up a pool")
            self.config_location.hide()

    def _encryption_changed(self, encrypted: bool) -> None:
        self.create_button.setText("Create encrypted pool" if encrypted else "Create pool")
        if not self.members or all(member.get("planned") for member in self.members):
            self.require_check.setChecked(encrypted)
        self.encryption_note.setText(
            "A password will be required to unlock these images."
            if encrypted else "Unencrypted member images can be read without a password."
        )
        self.encryption_note.setStyleSheet(
            "color:#9db0c9;" if encrypted else "color:#efbd79;"
        )

    def _error(self, text: str) -> None:
        self._message(text)
        QMessageBox.critical(self, "PoolFS Manager", text)

    def _run(self, work: Callable[[], Any], complete: Callable[[Any], None],
             failure: Callable[[str], None] | None = None) -> None:
        worker = Task(work, self)
        self._tasks.add(worker)
        worker.result.connect(complete)
        worker.failed.connect(failure if failure is not None else self._error)
        worker.finished.connect(lambda: self._tasks.discard(worker))
        worker.finished.connect(worker.deleteLater)
        worker.start()

    def _run_operation(
        self, activity: str, work: Callable[[], Any],
        complete: Callable[[Any], None],
        failure: Callable[[str], None] | None = None,
        show_progress: bool = False,
    ) -> None:
        if self._running_operation or (self.workspace and self.workspace.operation_in_progress()):
            self._message("Wait for the current PoolFS operation to finish.")
            return
        self._running_operation = True
        if show_progress:
            self.create_progress.setRange(0, 0)
            self.create_progress.show()
        self.centralWidget().setEnabled(False)
        self._message(activity)

        def finish() -> None:
            self._running_operation = False
            if show_progress:
                self.create_progress.hide()
            self.centralWidget().setEnabled(True)
            if self.workspace:
                self.workspace.refresh_mounts()
            else:
                self._refresh_mount_state()

        def succeeded(result: Any) -> None:
            finish()
            complete(result)

        def failed(message: str) -> None:
            finish()
            if failure is None:
                self._error(message)
            else:
                failure(message)

        worker = Task(work, self)
        self._tasks.add(worker)
        worker.result.connect(succeeded)
        worker.failed.connect(failed)
        worker.finished.connect(lambda: self._tasks.discard(worker))
        worker.finished.connect(worker.deleteLater)
        worker.start()

    def closeEvent(self, event: Any) -> None:
        if self._tasks:
            event.ignore()
            self._message("Wait for the current PoolFS operation to finish before closing.")
            return
        super().closeEvent(event)

    def load_inventory_sync(self) -> None:
        if self.backend is None:
            self._message("Native PoolFS bridge is unavailable.")
            return
        try:
            self._on_inventory(self.backend.inventory())
        except Exception as exc:
            self._message(str(exc))

    def refresh_inventory(self) -> None:
        if self._inventory_loading:
            return
        if self.backend is None:
            self._message("Native PoolFS bridge is unavailable.")
            return
        self._inventory_loading = True
        self._message("Reading physical disks and host volumes…")

        def failed(message: str) -> None:
            self._inventory_loading = False
            self._error(message)

        self._run(self.backend.inventory, self._on_inventory, failed)

    @staticmethod
    def _put(widget: QTableWidget, row: int, column: int, text: str, data: Any = None) -> None:
        item = QTableWidgetItem(text)
        if data is not None:
            item.setData(Qt.ItemDataRole.UserRole, data)
        widget.setItem(row, column, item)

    def _on_inventory(self, data: dict[str, Any]) -> None:
        self._inventory_loading = False
        self._inventory_loaded = True
        self.inventory = data
        disks = data.get("disks", [])
        volumes = data.get("volumes", [])
        self.host_volumes = [
            volume for volume in volumes
            if any(len(path) >= 3 and path[1:3] == ":\\"
                   for path in volume.get("mountPaths") or [])
            and volume.get("diskNumber") is not None
            and volume.get("partitionNumber") is not None
        ]
        self.disk_count.setText(str(len(disks)))
        self.partition_count.setText(str(sum(len(d.get("partitions", [])) for d in disks)))
        self.volume_count.setText(str(sum(bool(v.get("filesystem")) for v in self.host_volumes)))
        self.disks_table.setRowCount(len(disks))
        for row, disk in enumerate(disks):
            self._put(self.disks_table, row, 0, f"Disk {disk.get('number', '?')}", row)
            self._put(self.disks_table, row, 1, human_size(disk.get("totalBytes")))
            self._put(self.disks_table, row, 2,
                      {0: "MBR", 1: "GPT", 2: "RAW"}.get(disk.get("partitionStyle"), "Unknown"))
            self._put(self.disks_table, row, 3, str(disk.get("diskGuid") or disk.get("queryError") or ""))
            self.disks_table.setRowHeight(row, 40)
        self.partitions_table.setRowCount(0)
        self.partition_context.setText("Choose a disk on the left.")
        self.volumes_table.blockSignals(True)
        self.volumes_table.setRowCount(len(self.host_volumes))
        self.volumes_table.setFixedHeight(min(360, max(120, 43 + len(self.host_volumes) * 38)))
        for row, volume in enumerate(self.host_volumes):
            paths = volume.get("mountPaths") or []
            mount = next((p for p in paths if len(p) >= 2 and p[1] == ":"), None)
            mount = mount or (paths[0] if paths else "(no mount path)")
            filesystem = str(volume.get("filesystem") or "Unformatted")
            location = (
                f"Disk {volume['diskNumber']} / Part {volume['partitionNumber']}"
                if volume.get("diskNumber") is not None and volume.get("partitionNumber") is not None
                else "Unknown"
            )
            check = QTableWidgetItem()
            selectable = bool(volume.get("filesystem")) and (volume.get("freeBytes") or 0) >= 384 * MIB
            check.setFlags(
                Qt.ItemFlag.ItemIsUserCheckable |
                (Qt.ItemFlag.ItemIsEnabled if selectable else Qt.ItemFlag(0))
            )
            check.setCheckState(Qt.CheckState.Unchecked)
            check.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            if not selectable:
                check.setToolTip("Format this partition or free at least 384 MiB to use it.")
            self.volumes_table.setItem(row, 0, check)
            self._put(self.volumes_table, row, 1, mount, row)
            self._put(self.volumes_table, row, 2, filesystem)
            self._put(self.volumes_table, row, 3, human_size(volume.get("freeBytes")))
            self._put(self.volumes_table, row, 4, location)
            self._put(self.volumes_table, row, 5, str(volume.get("label") or ""))
            self.volumes_table.setRowHeight(row, 38)
        self.volumes_table.blockSignals(False)
        self.volume_selection.setText("No host volumes selected.")
        self.queue_button.setEnabled(False)
        self.queue_button.setText("Select host volumes")
        self.admin_button.setEnabled(not bool(data.get("partitionElevated")))
        self._update_partition_buttons()
        self._message(f"Ready · {len(disks)} disks · {len(volumes)} volumes")

    def _selected_disk(self) -> dict[str, Any] | None:
        row = self.disks_table.currentRow()
        disks = self.inventory.get("disks", [])
        if row < 0 or row >= len(disks):
            return None
        return disks[row]

    def _selected_partition(self) -> dict[str, Any] | None:
        disk = self._selected_disk()
        row = self.partitions_table.currentRow()
        if disk is None or row < 0:
            return None
        parts = disk.get("partitions", [])
        return parts[row] if row < len(parts) else None

    def _volume_for_part(self, disk_number: int, part_number: int) -> dict[str, Any] | None:
        return next(
            (v for v in self.inventory.get("volumes", [])
             if v.get("diskNumber") == disk_number and v.get("partitionNumber") == part_number),
            None,
        )

    def _selected_disk_changed(self) -> None:
        disk = self._selected_disk()
        if disk is None:
            self.partitions_table.setRowCount(0)
            self.partition_context.setText("Choose a disk on the left.")
            self._update_partition_buttons()
            return
        self.partition_context.setText(f"Disk {disk.get('number')} · select a partition for partition tools.")
        parts = disk.get("partitions", [])
        self.partitions_table.setRowCount(len(parts))
        for row, part in enumerate(parts):
            volume = self._volume_for_part(disk["number"], part["number"])
            paths = volume.get("mountPaths") or [] if volume else []
            drive = next((p[:2] for p in paths if len(p) >= 2 and p[1] == ":"), "—")
            self._put(self.partitions_table, row, 0, str(part.get("number", "?")), row)
            self._put(self.partitions_table, row, 1, drive)
            self._put(self.partitions_table, row, 2,
                      str(volume.get("filesystem") or "Unformatted") if volume else "—")
            self._put(self.partitions_table, row, 3, human_size(part.get("lengthBytes")))
            self._put(self.partitions_table, row, 4,
                      str((volume or {}).get("label") or part.get("name") or part.get("type") or ""))
            self.partitions_table.setRowHeight(row, 38)
        self._update_partition_buttons()

    def _checked_volume_rows(self) -> list[int]:
        return [
            row for row in range(self.volumes_table.rowCount())
            if self.volumes_table.item(row, 0).checkState() == Qt.CheckState.Checked
        ]

    def _volume_cell_clicked(self, row: int, column: int) -> None:
        if column != 0:
            item = self.volumes_table.item(row, 0)
            if not item.flags() & Qt.ItemFlag.ItemIsEnabled:
                return
            item.setCheckState(
                Qt.CheckState.Unchecked if item.checkState() == Qt.CheckState.Checked
                else Qt.CheckState.Checked
            )

    def _volume_selection_changed(self) -> None:
        rows = self._checked_volume_rows()
        replacing = bool(self.config_path) or any(not member.get("planned") for member in self.members)
        remaining = 16 if replacing else 16 - len(self.members)
        valid = bool(rows) and len(rows) <= remaining
        self.queue_button.setEnabled(valid)
        self.queue_button.setText(
            "Too many volumes" if len(rows) > remaining else
            f"Continue with {len(rows)} volume{'s' if len(rows) != 1 else ''}  →"
            if rows else "Select host volumes"
        )
        self.volume_selection.setText(
            f"Select no more than {remaining} host volume{'s' if remaining != 1 else ''} for this pool."
            if len(rows) > remaining else
            f"{len(rows)} host volume{'s' if len(rows) != 1 else ''} selected. "
            "One new image will be queued on each."
            if rows else "No host volumes selected."
        )

    def _refresh_member_controls(self) -> None:
        room = len(self.members) < 16
        self.add_existing_button.setEnabled(room)
        self.new_image_button.setEnabled(room)
        self.remove_member_button.setEnabled(
            bool(self.members_table.selectionModel().selectedRows())
        )

    def _member_host(self, member: dict[str, Any]) -> tuple[str, str, str]:
        """Return the host root, stable volume identity, and host filesystem."""
        if member.get("hostRoot"):
            root = str(member["hostRoot"])
            return root, str(member.get("hostGuid") or root).casefold(), str(member.get("hostFilesystem") or "")
        path = Path(str(member["path"]))
        if not path.is_absolute():
            raise OSError(f"Use an absolute image path: {path}")
        ancestor = path.parent
        while not ancestor.exists() and ancestor != ancestor.parent:
            ancestor = ancestor.parent
        if not ancestor.exists():
            raise OSError(f"The host volume for {path} is unavailable.")
        ancestor = ancestor.resolve()
        if os.name != "nt":
            root = str(ancestor.anchor)
            return root, root.casefold(), ""
        kernel32 = ctypes.windll.kernel32
        root_buffer = ctypes.create_unicode_buffer(32768)
        get_root = kernel32.GetVolumePathNameW
        get_root.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32]
        get_root.restype = ctypes.c_int
        if not get_root(str(ancestor), root_buffer, len(root_buffer)):
            raise OSError(f"Could not identify the host volume for {path}.")
        root = root_buffer.value
        guid_buffer = ctypes.create_unicode_buffer(128)
        get_guid = kernel32.GetVolumeNameForVolumeMountPointW
        get_guid.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32]
        get_guid.restype = ctypes.c_int
        key = guid_buffer.value if get_guid(root, guid_buffer, len(guid_buffer)) else root
        filesystem_buffer = ctypes.create_unicode_buffer(64)
        get_filesystem = kernel32.GetVolumeInformationW
        get_filesystem.argtypes = [
            ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32,
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
            ctypes.c_wchar_p, ctypes.c_uint32,
        ]
        get_filesystem.restype = ctypes.c_int
        if not get_filesystem(root, None, 0, None, None, None,
                              filesystem_buffer, len(filesystem_buffer)):
            raise OSError(f"Could not read the host filesystem for {path}.")
        return root, key.casefold(), filesystem_buffer.value

    def _size_limit_for_member(self, row: int) -> int:
        member = self.members[row]
        root, key, filesystem = self._member_host(member)
        other_mib = sum(
            int(other.get("sizeMiB", MIN_IMAGE_MIB))
            for index, other in enumerate(self.members)
            if index != row and other.get("planned") and self._member_host(other)[1] == key
        )
        free_mib = shutil.disk_usage(root).free // MIB
        maximum = min(MAX_IMAGE_MIB, free_mib - other_mib)
        if filesystem.upper() == "FAT32":
            maximum = min(maximum, 0xFFFFFFFF // MIB)
        return maximum

    def _planned_sizes_fit(self) -> bool:
        requested: dict[str, tuple[str, int]] = {}
        try:
            for member in self.members:
                if not member.get("planned"):
                    continue
                root, key, filesystem = self._member_host(member)
                size_mib = int(member.get("sizeMiB", 0))
                if size_mib < MIN_IMAGE_MIB or size_mib > MAX_IMAGE_MIB:
                    return False
                if filesystem.upper() == "FAT32" and size_mib > 0xFFFFFFFF // MIB:
                    return False
                old = requested.get(key, (root, 0))[1]
                requested[key] = root, old + size_mib
            return all(shutil.disk_usage(root).free // MIB >= total
                       for root, total in requested.values())
        except (OSError, ValueError):
            return False

    def _refresh_create_button(self) -> None:
        try:
            validate_pool_name(self.pool_name.text())
            valid_name = True
        except ValueError:
            valid_name = False
        planned = [member for member in self.members if member.get("planned")]
        self.create_button.setEnabled(
            self.backend is not None and valid_name and bool(planned) and
            len(planned) == len(self.members) and
            not any(Path(str(member["path"])).exists() for member in planned) and
            self._planned_sizes_fit()
        )

    def _refresh_mount_state(self) -> None:
        if self.backend is None:
            self._mounted_drives = None
        else:
            try:
                drives = self.backend.mounts().get("mountedDrives", [])
                self._mounted_drives = {str(drive).upper() for drive in drives}
            except Exception:
                # An unknown mount state must not enable a destructive or offline action.
                self._mounted_drives = None
        self._apply_mount_state(self._mounted_drives)

    def _apply_mount_state(self, drives: set[str] | None) -> None:
        self._mounted_drives = drives
        self._refresh_pool_actions()
        self._refresh_stats_drives()

    def _clear_stats(self) -> None:
        for value in (self.stats_read_rate, self.stats_write_rate,
                      self.stats_total_read, self.stats_total_write,
                      self.stats_read_ops, self.stats_write_ops):
            value.setText("—")
        self.stats_capacity.setText("Mount a pool to see its available space.")
        self.stats_usage.setValue(0)
        self.stats_graph.clear()

    def _stats_drive_changed(self) -> None:
        self._clear_stats()
        drive = self.stats_drive.currentText()
        if drive:
            self._stats_previous.pop(drive, None)
            self.stats_status.setText("Measuring activity on " + drive + "…")
        else:
            self.stats_status.setText("Mount a pool to see live statistics.")

    def _refresh_stats_drives(self) -> None:
        if not hasattr(self, "stats_drive"):
            return
        drives = sorted(self._mounted_drives or [])
        selected = self.stats_drive.currentText()
        if drives == [self.stats_drive.itemText(i) for i in range(self.stats_drive.count())]:
            self.stats_drive.setEnabled(bool(drives))
            if not drives and self._mounted_drives is None:
                self.stats_status.setText("Could not inspect mounted pools. Try Refresh.")
            return
        self.stats_drive.blockSignals(True)
        self.stats_drive.clear()
        self.stats_drive.addItems(drives)
        if selected in drives:
            self.stats_drive.setCurrentText(selected)
        self.stats_drive.blockSignals(False)
        self.stats_drive.setEnabled(bool(drives))
        if self.stats_drive.currentText() != selected:
            self._stats_drive_changed()
        if not drives and self._mounted_drives is None:
            self.stats_status.setText("Could not inspect mounted pools. Try Refresh.")

    def _refresh_stats(self) -> None:
        self._refresh_mount_state()
        self._poll_stats()

    def _poll_stats(self) -> None:
        if self.workspace and self.workspace.pool_stack.currentWidget() is not self:
            return
        if self.pages.currentIndex() != 3 or self.backend is None:
            return
        drive = self.stats_drive.currentText()
        if not drive:
            return
        try:
            sample = self.backend.stats(drive)
        except Exception as exc:
            self._clear_stats()
            self.stats_status.setText(str(exc))
            return
        now = time.monotonic()
        read_bytes = int(sample["readBytes"])
        write_bytes = int(sample["writeBytes"])
        uptime = int(sample["uptimeMs"])
        prior = self._stats_previous.get(drive)
        read_rate = write_rate = 0.0
        if prior and uptime >= prior[3] and read_bytes >= prior[1] and write_bytes >= prior[2]:
            elapsed = now - prior[0]
            if elapsed >= 0.05:
                read_rate = (read_bytes - prior[1]) / elapsed
                write_rate = (write_bytes - prior[2]) / elapsed
        elif prior:
            self.stats_graph.clear()  # The drive was remounted; counters restarted.
        self._stats_previous[drive] = (now, read_bytes, write_bytes, uptime)
        self.stats_read_rate.setText(human_rate(read_rate) if prior else "Measuring…")
        self.stats_write_rate.setText(human_rate(write_rate) if prior else "Measuring…")
        self.stats_total_read.setText(human_size(read_bytes))
        self.stats_total_write.setText(human_size(write_bytes))
        self.stats_read_ops.setText(f"{int(sample['readOperations']):,}")
        self.stats_write_ops.setText(f"{int(sample['writeOperations']):,}")
        capacity = int(sample["capacityBytes"])
        available = int(sample["availableBytes"])
        used = max(0, capacity - available)
        self.stats_capacity.setText(
            f"{human_size(used)} used · {human_size(available)} available · "
            f"{human_size(capacity)} total"
        )
        self.stats_usage.setValue(min(100, int(used * 100 / capacity)) if capacity else 0)
        self.stats_graph.add_sample(read_rate, write_rate)
        hours, remainder = divmod(uptime // 1000, 3600)
        minutes = remainder // 60
        self.stats_status.setText(
            f"Updated {datetime.now():%H:%M:%S} · mounted for {hours}h {minutes}m · "
            "rates are completed file I/O, excluding metadata and physical disk traffic."
        )

    @staticmethod
    def _drive_available(drive: str) -> bool:
        if os.name != "nt":
            return True
        get_drive_type = ctypes.windll.kernel32.GetDriveTypeW
        get_drive_type.argtypes = [ctypes.c_wchar_p]
        get_drive_type.restype = ctypes.c_uint
        return get_drive_type(f"{drive[0]}:\\") == 1

    def _suggest_free_drive(self) -> str:
        drives = ["P:"] + [f"{chr(code)}:" for code in range(ord("D"), ord("Z") + 1)
                            if code != ord("P")]
        reserved = self.workspace.reserved_drives(self) if self.workspace else set()
        available = [drive for drive in drives if self._drive_available(drive)]
        return next((drive for drive in available if drive not in reserved),
                    available[0] if available else "P:")

    def _mount_matches(self, drive: str) -> bool | None:
        if self.backend is None or not self.config or self.config["drive"] != drive:
            return None
        try:
            return self.backend.match_mount(drive, self.config)
        except (BridgeError, OSError):
            return None

    def _refresh_pool_actions(self) -> None:
        if not hasattr(self, "create_button"):
            return
        drive = self.drive_combo.currentText().upper()
        occupied_by_poolfs = self._mounted_drives is not None and drive in self._mounted_drives
        match = self._mount_matches(drive) if occupied_by_poolfs else None
        mounted = match is True
        unmounted = self._mounted_drives is not None and not occupied_by_poolfs
        drive_available = self._drive_available(drive)
        missing = sum(not Path(str(member["path"])).is_file()
                      for member in self.members if not member.get("planned"))
        planned = any(member.get("planned") for member in self.members)
        existing = bool(self.members) and not planned and not missing
        current = {
            "name": self.pool_name.text(),
            "drive": self.drive_combo.currentText(),
            "requireEncrypted": self.require_check.isChecked(),
            "members": [str(member["path"]) for member in self.members],
        }
        try:
            valid_name = bool(validate_pool_name(self.pool_name.text()))
        except ValueError:
            valid_name = False
        saved = bool(self.config_path and Path(self.config_path).is_file()) and current == self.config
        ready = self.backend is not None and existing and saved
        self.mount_indicator.setVisible(bool(self.config_path) or occupied_by_poolfs)
        if mounted:
            status, color, background = "PoolFS mounted", "#a0f1d4", "#15343d"
        elif occupied_by_poolfs:
            status, color, background = (
                "Different pool mounted" if match is False else "PoolFS mount unverified",
                "#f4d3a3", "#403324",
            )
        elif self._mounted_drives is None:
            status, color, background = "Status unavailable", "#ccd9e8", "#24354d"
        elif not drive_available:
            status, color, background = "Drive in use", "#f4d3a3", "#403324"
        else:
            status, color, background = "Unmounted", "#ccd9e8", "#24354d"
        indicator = f"{drive} · {status}"
        if self.mount_indicator.text() != indicator:
            self.mount_indicator.setText(indicator)
            self.mount_indicator.setStyleSheet(
                f"color:{color};background:{background};border-radius:8px;"
                "padding:3px 8px;font-weight:600;"
            )
        can_save = (self.backend is not None and existing and valid_name and
                    (not self.config_path or unmounted))
        self.save_button.setEnabled(can_save)
        self.save_recovered_button.setVisible(existing and not self.config_path)
        self.save_recovered_button.setEnabled(can_save)
        self.actions_card.setVisible(bool(self.config_path))
        self.create_button.setVisible(planned)
        self.mount_button.setVisible(bool(self.config_path) and not mounted)
        self.unmount_button.setVisible(mounted)
        self.save_button.setVisible(bool(self.config_path))
        self.mount_button.setEnabled(ready and unmounted and drive_available)
        self.verify_button.setEnabled(ready and unmounted)
        self.grow_button.setEnabled(ready and unmounted)
        self.reclaim_button.setEnabled(ready and mounted)
        self.unmount_button.setEnabled(self.backend is not None and mounted)
        if not self.members:
            guidance = "Choose where to store new member images, or open a saved config."
        elif planned:
            guidance = (
                "A new pool cannot mix existing and new images. Start a new pool draft."
                if not all(member.get("planned") for member in self.members) else
                "Next: select each new image to set its size, then create the pool."
            )
        elif not existing:
            guidance = f"{missing} member image{'s' if missing != 1 else ''} missing. Connect its host volume or update the path."
        elif not self.config_path:
            guidance = "Member images are ready. Name the pool, then save a config to mount."
        elif not saved:
            guidance = "Config or member details changed. Save a config before mounting."
        elif mounted:
            guidance = "Use Unmount before disconnecting a member drive."
        elif occupied_by_poolfs:
            guidance = f"Choose a free drive letter, or open the config for the pool mounted on {drive}."
        elif unmounted and not drive_available:
            guidance = f"{drive} is already in use. Choose a free drive letter before mounting."
        elif unmounted:
            guidance = f"Ready to mount on {drive}. All member images are present."
        else:
            guidance = "Checking whether this pool is mounted…"
        self.pool_guidance.setText(guidance)
        if self.workspace:
            self.workspace.update_pool_entry(self, "Missing images" if missing and not mounted else status)

    def _refresh_startup_option(self) -> None:
        path = self.config_path
        available = (os.name == "nt" and self.backend is not None and
                     bool(path and Path(path).is_file()))
        self.startup_check.blockSignals(True)
        try:
            self.startup_check.setChecked(bool(path and startup_is_enabled(path)))
            self.startup_check.setEnabled(available)
        except OSError:
            self.startup_check.setChecked(False)
            self.startup_check.setEnabled(False)
        finally:
            self.startup_check.blockSignals(False)

    def _startup_changed(self, enabled: bool) -> None:
        if not self.config_path:
            return
        try:
            set_startup_enabled(self.config_path, enabled)
        except (OSError, RuntimeError, ValueError) as exc:
            self._refresh_startup_option()
            self._error(str(exc))
            return
        self._message(
            "This pool will mount when you sign in to Windows."
            if enabled else "Automatic mount at sign-in is off for this pool."
        )

    def _slider_value_for_size(self, size_mib: int, maximum: int) -> int:
        if maximum <= 2_147_483_647:
            return max(MIN_IMAGE_MIB, min(size_mib, maximum))
        return max(0, min(MAX_SLIDER_STEPS,
                          (size_mib - MIN_IMAGE_MIB) * MAX_SLIDER_STEPS //
                          (maximum - MIN_IMAGE_MIB)))

    def _size_for_slider_value(self, value: int, maximum: int) -> int:
        if maximum <= 2_147_483_647:
            return value
        return MIN_IMAGE_MIB + (
            value * (maximum - MIN_IMAGE_MIB) + MAX_SLIDER_STEPS // 2
        ) // MAX_SLIDER_STEPS

    def _refresh_size_editor(self) -> None:
        row = self.members_table.currentRow()
        old_row = getattr(self, "_size_edit_row", None)
        old_member = getattr(self, "_size_edit_member", None)
        if (old_row != row and self.size_input.isModified() and
                old_row is not None and 0 <= old_row < len(self.members) and
                self.members[old_row] is old_member):
            self.size_input.setModified(False)
            self._commit_size_input(old_row)
        self._size_edit_row = row
        self._size_edit_member = self.members[row] if 0 <= row < len(self.members) else None
        self._displayed_size_maximum = None
        self.size_slider.blockSignals(True)
        self.size_input.blockSignals(True)
        try:
            if not 0 <= row < len(self.members) or not self.members[row].get("planned"):
                self.size_selected.setText("Select a new image above")
                self.size_slider.setEnabled(False)
                self.size_input.setEnabled(False)
                self.size_input.clear()
                self.size_maximum.setText("Select a new member image to set its size.")
                return
            member = self.members[row]
            self.size_selected.setText(Path(str(member["path"])).name)
            self.size_selected.setToolTip(str(member["path"]))
            size_mib = int(member.get("sizeMiB", MIN_IMAGE_MIB))
            self.size_input.setText(str(size_mib))
            try:
                maximum = self._size_limit_for_member(row)
            except OSError as exc:
                self.size_slider.setEnabled(False)
                self.size_input.setEnabled(False)
                self.size_maximum.setText(f"Free space unavailable: {exc}")
                return
            if maximum < MIN_IMAGE_MIB:
                self.size_slider.setEnabled(False)
                self.size_input.setEnabled(False)
                self.size_maximum.setText(
                    f"Only {max(0, maximum):,} MiB available after other images. "
                    f"Free up space or remove another image (minimum {MIN_IMAGE_MIB} MiB)."
                )
                return
            self._displayed_size_maximum = maximum
            self.size_slider.setEnabled(True)
            self.size_input.setEnabled(True)
            self.size_slider.setRange(
                MIN_IMAGE_MIB if maximum <= 2_147_483_647 else 0,
                maximum if maximum <= 2_147_483_647 else MAX_SLIDER_STEPS,
            )
            self.size_slider.setValue(self._slider_value_for_size(size_mib, maximum))
            self.size_maximum.setText(
                f"Minimum {MIN_IMAGE_MIB} MiB · Maximum available here: "
                f"{maximum:,} MiB ({human_size(maximum * MIB)})."
                + (" Current size exceeds available space." if size_mib > maximum else "")
            )
        finally:
            self.size_slider.blockSignals(False)
            self.size_input.blockSignals(False)

    def _set_member_size(self, row: int, size_mib: int, maximum: int) -> None:
        self.members[row]["sizeMiB"] = size_mib
        item = self.members_table.item(row, 1)
        if item is not None:
            item.setText(f"{size_mib:,} MiB")
        self.size_input.setText(str(size_mib))
        self.size_slider.blockSignals(True)
        try:
            self.size_slider.setValue(self._slider_value_for_size(size_mib, maximum))
        finally:
            self.size_slider.blockSignals(False)
        self._size_validation_timer.start(120)

    def _size_slider_changed(self, value: int) -> None:
        row = self.members_table.currentRow()
        if not 0 <= row < len(self.members) or not self.members[row].get("planned"):
            return
        try:
            maximum = self._size_limit_for_member(row)
        except OSError:
            self._refresh_size_editor()
            self._refresh_create_button()
            return
        if maximum < MIN_IMAGE_MIB:
            self._refresh_size_editor()
            self._refresh_create_button()
            return
        if maximum != getattr(self, "_displayed_size_maximum", maximum):
            self._refresh_size_editor()
            return
        size_mib = max(MIN_IMAGE_MIB, min(self._size_for_slider_value(value, maximum), maximum))
        self._set_member_size(row, size_mib, maximum)

    def _size_input_finished(self) -> None:
        row = getattr(self, "_size_edit_row", None)
        if row is None or not 0 <= row < len(self.members):
            return
        if self.members[row] is not getattr(self, "_size_edit_member", None):
            return
        self._commit_size_input(row)

    def _commit_size_input(self, row: int) -> None:
        if not self.members[row].get("planned"):
            return
        try:
            maximum = self._size_limit_for_member(row)
        except OSError:
            self._refresh_size_editor()
            self._refresh_create_button()
            return
        if maximum < MIN_IMAGE_MIB:
            self._refresh_size_editor()
            self._refresh_create_button()
            return
        try:
            requested = int(self.size_input.text())
        except ValueError:
            self._refresh_size_editor()
            return
        size_mib = max(MIN_IMAGE_MIB, min(requested, maximum))
        self._set_member_size(row, size_mib, maximum)
        if requested != size_mib:
            self._message(f"Image size adjusted to the available range: {size_mib:,} MiB.")

    def _refresh_members(self) -> None:
        selected_row = self.members_table.currentRow()
        self.member_empty.setVisible(not bool(self.members))
        self.members_table.setVisible(bool(self.members))
        self.members_table.blockSignals(True)
        self.members_table.setRowCount(len(self.members))
        self.members_table.setFixedHeight(min(325, max(135, 37 + len(self.members) * 39)))
        for row, member in enumerate(self.members):
            path = str(member["path"])
            self._put(self.members_table, row, 0, path, row)
            self.members_table.item(row, 0).setToolTip(path)
            if member.get("planned"):
                self._put(self.members_table, row, 1,
                          f"{int(member.get('sizeMiB', MIN_IMAGE_MIB)):,} MiB")
            else:
                self._put(self.members_table, row, 1, human_size(Path(path).stat().st_size)
                          if Path(path).is_file() else "Missing")
            self._put(self.members_table, row, 2,
                      "Created during failed format" if member.get("partial") else
                      "New path" if member.get("planned") else
                      "Present" if Path(path).is_file() else "Missing")
            self.members_table.setRowHeight(row, 39)
        if self.members:
            if not 0 <= selected_row < len(self.members):
                selected_row = next((i for i, m in enumerate(self.members)
                                     if m.get("planned")), 0)
            self.members_table.selectRow(selected_row)
        self.members_table.blockSignals(False)
        self._refresh_size_editor()
        self._refresh_member_controls()
        self.member_count.setText(f"{len(self.members)} of 16 member images")
        self._refresh_pool_identity()
        self._refresh_startup_option()
        planned = [m for m in self.members if m.get("planned")]
        self.size_frame.setVisible(bool(planned))
        existing = len(self.members) - len(planned)
        self.choose_volumes_button.setVisible(not existing)
        self.choose_volumes_button.setText(
            "Add host volumes" if planned else "Choose host volumes"
        )
        self.encrypt_check.setVisible(not existing)
        self.encryption_note.setVisible(not existing)
        if not existing:
            self.encryption_note.setText(
                "A password will be required to unlock these images."
                if self.encrypt_check.isChecked()
                else "Unencrypted member images can be read without a password."
            )
            self.encryption_note.setStyleSheet(
                "color:#9db0c9;" if self.encrypt_check.isChecked()
                else "color:#efbd79;"
            )
        self.require_check.setEnabled(not planned)
        self.require_check.setVisible(bool(existing))
        self.startup_check.setVisible(bool(self.config_path))
        self._refresh_create_button()
        self._refresh_mount_state()

    def _same_mount(self, path: str, expected: str) -> bool:
        if os.name != "nt":
            return False
        buffer = ctypes.create_unicode_buffer(128)
        get_volume_name = ctypes.windll.kernel32.GetVolumeNameForVolumeMountPointW
        get_volume_name.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32]
        get_volume_name.restype = ctypes.c_int
        ok = get_volume_name(
            ctypes.c_wchar_p(path), buffer, len(buffer)
        )
        return bool(ok) and buffer.value.casefold() == expected.casefold()

    def queue_volumes(self) -> None:
        rows = self._checked_volume_rows()
        if not rows:
            return
        selected = [self.host_volumes[row] for row in rows]
        existing = self.config_path or any(not m.get("planned") for m in self.members)
        if existing:
            answer = QMessageBox.question(
                self, "Start a new pool",
                "PoolFS cannot add members to an existing formatted pool. "
                "Start a separate new pool with the selected volumes? "
                "Existing image files and saved configs stay intact."
            )
            if answer != QMessageBox.StandardButton.Yes:
                return
        current_members = [] if existing else self.members
        if len(current_members) + len(selected) > 16:
            self._error("A PoolFS pool supports at most 16 member images.")
            return
        planned: list[dict[str, Any]] = []
        used_names = {Path(str(member["path"])).name.casefold()
                      for member in current_members}
        next_number = 1
        for volume in selected:
            if volume.get("diskNumber") is None or volume.get("partitionNumber") is None:
                self._error("Select a physical host volume; PoolFS virtual volumes cannot host a member image.")
                return
            if not volume.get("filesystem"):
                self._error("A selected host partition is unformatted. Format it before adding an image.")
                return
            paths = volume.get("mountPaths") or []
            root = next((p for p in paths if len(p) >= 3 and p[1:3] == ":\\"), None)
            if root is None:
                self._error("A selected host volume needs a drive letter before it can hold an image.")
                return
            if not self._same_mount(root, volume.get("guidPath") or ""):
                self._error("A selected volume changed. Refresh Storage and choose it again.")
                return
            try:
                reserved = sum(
                    int(member.get("sizeMiB", MIN_IMAGE_MIB))
                    for member in [*current_members, *planned]
                    if member.get("planned") and
                    self._member_host(member)[1] == str(volume["guidPath"]).casefold()
                )
                available_mib = shutil.disk_usage(root).free // MIB - reserved
            except OSError as exc:
                self._error(f"Could not read host free space: {exc}")
                return
            if available_mib < MIN_IMAGE_MIB:
                self._error("A selected volume needs at least 384 MiB free after its other planned images.")
                return
            folder = Path(root) / "PoolFS"
            candidate = None
            for number in range(next_number, 10001):
                option = folder / f"member-{number}.img"
                if option.name.casefold() not in used_names and not option.exists():
                    candidate = option
                    used_names.add(option.name.casefold())
                    next_number = number + 1
                    break
            if candidate is None:
                self._error("Could not find an unused member image name.")
                return
            planned.append({
                "path": str(candidate), "sizeMiB": 384, "planned": True,
                "hostRoot": root, "hostGuid": volume["guidPath"],
                "hostFilesystem": volume["filesystem"],
            })
        if existing:
            if self.workspace:
                target = self.workspace.add_pool()
                target._on_inventory(self.inventory)
            else:
                self.members.clear()
                self.config = None
                self.config_path = None
                self.pool_name.setText(NEW_POOL_NAME)
                self.encrypt_check.setChecked(False)
                self.require_check.setChecked(False)
                target = self
        else:
            target = self
        target.members.extend(planned)
        for row in rows:
            self.volumes_table.item(row, 0).setCheckState(Qt.CheckState.Unchecked)
        target._refresh_members()
        target.members_table.selectRow(len(target.members) - len(planned))
        target._set_page("Pool")
        target._message(f"Queued {len(planned)} new image path(s). Set sizes, then create the pool.")

    def new_pool(self) -> None:
        if self.workspace and (self.config_path or self.members):
            self.workspace.add_pool()
            return
        if self.members:
            answer = QMessageBox.question(
                self, "New pool", "Clear the current draft? "
                "No existing image file or saved config will be deleted."
            )
            if answer != QMessageBox.StandardButton.Yes:
                return
        self.members.clear()
        self.config = None
        self.config_path = None
        self.drive_combo.setCurrentText(self._suggest_free_drive() if self.workspace else "P:")
        self.pool_name.setText(NEW_POOL_NAME)
        self.encrypt_check.setChecked(False)
        self.require_check.setChecked(False)
        self._refresh_members()
        self._message("Started a new pool draft.")

    def open_config(self) -> None:
        if self.backend is None:
            self._error("Native PoolFS bridge is unavailable.")
            return
        if self.workspace:
            paths, _ = QFileDialog.getOpenFileNames(self, "Open PoolFS configs", "", "JSON files (*.json)")
            for path in paths:
                self.workspace.open_pool_config(path)
        else:
            path, _ = QFileDialog.getOpenFileName(self, "Open PoolFS config", "", "JSON files (*.json)")
            if path:
                self.load_config_path(path)

    def _recent_config_paths(self) -> list[str]:
        self.settings.sync()
        return self.settings.value(RECENT_CONFIGS_KEY, [], type=list)

    def _remember_config(self, path: str) -> None:
        path = os.path.abspath(path)
        key = os.path.normcase(path)
        older = [item for item in self._recent_config_paths()
                 if os.path.normcase(item) != key]
        self.settings.setValue(RECENT_CONFIGS_KEY, [path, *older][:RECENT_CONFIGS_LIMIT])
        self.settings.sync()

    def _refresh_recent_configs(self) -> None:
        self._populate_recent_configs(self.recent_configs_menu)

    def _populate_recent_configs(self, menu: QMenu) -> None:
        menu.clear()
        paths = self._recent_config_paths()
        for path in paths:
            title = menu.fontMetrics().elidedText(path, Qt.TextElideMode.ElideMiddle, 480)
            action = menu.addAction(title.replace("&", "&&"))
            action.setToolTip(path)
            open_path = self.workspace.open_pool_config if self.workspace else self.load_config_path
            action.triggered.connect(lambda _checked=False, selected=path: open_path(selected))
        if paths:
            menu.addSeparator()
            menu.addAction("Clear recent configs", self._clear_recent_configs)
        else:
            menu.addAction("No recent configs").setEnabled(False)

    def _clear_recent_configs(self) -> None:
        self.settings.remove(RECENT_CONFIGS_KEY)
        self.settings.sync()

    def load_config_path(self, path: str, *, remember: bool = True) -> bool:
        if self.backend is None:
            self._error("Native PoolFS bridge is unavailable.")
            return False
        try:
            config = self.backend.load_config(path)
            self.config = config
            self.config_path = path
            self.members = [
                {"path": member, "planned": False}
                for member in config.get("members", [])
            ]
            self.pool_name.setText(str(config.get("name") or "PoolFS"))
            self.drive_combo.setCurrentText(str(config["drive"]))
            self.require_check.setChecked(bool(config.get("requireEncrypted", True)))
            self._refresh_members()
            self._set_page("Pool")
            if remember:
                self._remember_config(path)
            self._message(f"Opened {Path(path).name}")
            return True
        except Exception as exc:
            self._error(str(exc))
            return False

    def add_existing_image(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Select PoolFS image", "", "Images (*.img);;All files (*)")
        if path:
            if any(str(m["path"]).casefold() == path.casefold() for m in self.members):
                self._error("This image is already in the member list.")
                return
            if len(self.members) >= 16:
                self._error("A pool supports at most 16 images.")
                return
            self.members.append({"path": path, "planned": False})
            self._refresh_members()

    def add_new_image_path(self) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "Choose new image path", "", "Images (*.img)")
        if path:
            if Path(path).exists():
                self._error("A new member image path must not exist yet.")
                return
            if any(str(m["path"]).casefold() == path.casefold() for m in self.members):
                self._error("This image path is already in the member list.")
                return
            if any(m.get("planned") and Path(str(m["path"])).name.casefold() ==
                   Path(path).name.casefold() for m in self.members):
                self._error("New member images in a pool must have different file names.")
                return
            if len(self.members) >= 16:
                self._error("A pool supports at most 16 images.")
                return
            self.members.append({"path": path, "sizeMiB": 384, "planned": True})
            self._refresh_members()
            self.members_table.selectRow(len(self.members) - 1)

    def remove_selected_member(self) -> None:
        row = self.members_table.currentRow()
        if 0 <= row < len(self.members):
            self.members.pop(row)
            self._refresh_members()

    def add_scanned_image(self) -> None:
        group = self._selected_image_group
        if not group:
            return
        paths = [str(image["path"]) for image in group]
        if self.workspace:
            self.workspace.open_discovered_pool(paths, bool(group[0].get("encrypted")))
            return
        current_paths = [str(member["path"]) for member in self.members]
        if ([path.casefold() for path in paths] !=
                [path.casefold() for path in current_paths] or
                any(member.get("planned") for member in self.members)):
            if self.members and QMessageBox.question(
                self, "Use discovered pool",
                "Replace the current pool selection with these discovered members? "
                "No image files or saved configs will be changed."
            ) != QMessageBox.StandardButton.Yes:
                return
            self._load_discovered_members(paths, bool(group[0].get("encrypted")))
        self._set_page("Pool")
        self._message(f"Loaded {len(group)} member image(s). Name the pool and save a config.")

    def _load_discovered_members(self, paths: list[str], encrypted: bool) -> None:
        self.members = [{"path": path, "planned": False} for path in paths]
        self.config = None
        self.config_path = None
        self.pool_name.setText(NEW_POOL_NAME)
        self.drive_combo.setCurrentText(self._suggest_free_drive())
        self.require_check.setChecked(encrypted)
        self._refresh_members()

    def _refresh_image_action(self) -> None:
        selected_rows = self.images_table.selectionModel().selectedRows()
        row = selected_rows[0].row() if selected_rows else -1
        self._selected_image_group = []
        if not 0 <= row < len(self.images):
            self.image_selection.setText("Select an image to see whether its complete pool is here.")
            self.image_action.setText("Select an image")
            self.image_action.setEnabled(False)
            return
        selected = self.images[row]
        pool_id = str(selected.get("poolId") or "")
        group = ([image for image in self.images if str(image.get("poolId") or "") == pool_id]
                 if pool_id else [selected])
        try:
            expected = int(selected.get("memberCount", 0))
            indexes = {int(image.get("memberIndex", -1)) for image in group}
        except (TypeError, ValueError):
            expected, indexes = 0, set()
        complete = (
            1 <= expected <= 16 and len(group) == expected and
            indexes == set(range(expected)) and
            len({str(image.get("path", "")).casefold() for image in group}) == expected and
            len({bool(image.get("encrypted")) for image in group}) == 1 and
            all(Path(str(image.get("path", ""))).is_file() for image in group)
        )
        if complete:
            self._selected_image_group = sorted(group, key=lambda image: int(image["memberIndex"]))
            self.image_selection.setText(
                f"All {expected} member image{'s' if expected != 1 else ''} found for this pool."
            )
            self.image_action.setText("Load complete pool  →")
        else:
            self.image_selection.setText(
                "Multiple copies of this pool were found. Open its config or add exact images manually."
                if expected and len(group) > expected else
                f"This pool needs {expected or '?'} member images; the complete set is not available here."
            )
            self.image_action.setText("Complete pool required")
        self.image_action.setEnabled(complete)

    def scan_images(self) -> None:
        if self.backend is None:
            return
        self._message("Scanning PoolFS folders…")
        self._run(self.backend.scan_images, self._on_images)

    def _on_images(self, result: dict[str, Any]) -> None:
        self._images_scanned = True
        self.images = sorted(result.get("images", []), key=lambda image: (
            str(image.get("poolId") or ""), int(image.get("memberIndex") or 0)
        ))
        self.images_table.setRowCount(len(self.images))
        self.images_table.setFixedHeight(min(390, max(120, 43 + len(self.images) * 39)))
        for row, image in enumerate(self.images):
            self._put(self.images_table, row, 0, str(image.get("path", "")), row)
            self._put(self.images_table, row, 1, human_size(image.get("bytes")))
            pool_id = str(image.get("poolId") or "")
            self._put(self.images_table, row, 2, pool_id[:8] + "…" if len(pool_id) > 8 else pool_id)
            self.images_table.item(row, 2).setToolTip(pool_id)
            self._put(self.images_table, row, 3,
                      f"{int(image.get('memberIndex', 0)) + 1} of {image.get('memberCount', '?')}")
            self._put(self.images_table, row, 4, "Yes" if image.get("encrypted") else "No")
            self.images_table.setRowHeight(row, 39)
        self.image_count.setText(f"{len(self.images)} member image(s) found")
        self.images_table.clearSelection()
        self._refresh_image_action()
        self._message(f"Image scan complete · {len(self.images)} found")

    def _password(self, title: str, confirm: bool = False) -> str | None:
        dialog = PasswordDialog(self, title, confirm)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            dialog.first.clear()
            if dialog.second is not None:
                dialog.second.clear()
            return None
        return dialog.take_password()

    def _call_sync(self, activity: str, action: Callable[[], Any]) -> Any | None:
        self._message(activity)
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        QApplication.processEvents()
        try:
            return action()
        except Exception as exc:
            self._error(str(exc))
            return None
        finally:
            QApplication.restoreOverrideCursor()

    def create_pool(self) -> None:
        if self.backend is None:
            self._error("Native PoolFS bridge is unavailable.")
            return
        try:
            name = validate_pool_name(self.pool_name.text())
        except ValueError as exc:
            self._error(str(exc))
            return
        if not self.members or any(not m.get("planned") for m in self.members):
            self._error("Create pool needs only new image paths. Start a new pool for existing images.")
            return
        if len(self.members) > 16:
            self._error("A pool supports at most 16 images.")
            return
        requested_by_host: dict[str, tuple[str, int]] = {}
        try:
            for member in self.members:
                path = Path(str(member["path"]))
                if path.exists():
                    self._error(f"New image path already exists: {path}")
                    return
                size_mib = int(member.get("sizeMiB", 0))
                if size_mib < MIN_IMAGE_MIB:
                    self._error("Each member image must be at least 384 MiB.")
                    return
                if size_mib > MAX_IMAGE_MIB:
                    self._error("The member image exceeds the PoolFS format size limit.")
                    return
                root, host_key, filesystem = self._member_host(member)
                if member.get("hostRoot"):
                    if not self._same_mount(root, str(member["hostGuid"])):
                        self._error("A selected host volume changed. Refresh Storage and select it again.")
                        return
                if filesystem.upper() == "FAT32" and size_mib > 0xFFFFFFFF // MIB:
                    self._error("A FAT32 host volume cannot hold this image size.")
                    return
                previous = requested_by_host.get(host_key, (root, 0))[1]
                requested_by_host[host_key] = (root, previous + size_mib * MIB)
            for root, requested_bytes in requested_by_host.values():
                if shutil.disk_usage(root).free < requested_bytes:
                    self._error(
                        f"Not enough free space on {root} for the planned images "
                        f"({human_size(requested_bytes)} requested)."
                    )
                    return
        except OSError as exc:
            self._error(f"Could not validate free space: {exc}")
            return
        total = sum(int(m["sizeMiB"]) for m in self.members)
        encrypted = self.encrypt_check.isChecked()
        mode = "encrypted" if encrypted else "unencrypted"
        warning = (
            "\n\nThese images will be readable without a password."
            if not encrypted else ""
        )
        if QMessageBox.question(
            self, "Create PoolFS pool",
            f"Create {name} with {len(self.members)} new {mode} image(s), "
            f"totaling {total:,} MiB? Existing files will not be overwritten."
            + warning
        ) != QMessageBox.StandardButton.Yes:
            return
        password = self._password("Create encrypted pool", True) if encrypted else None
        if encrypted and password is None:
            return
        planned_members = [dict(member) for member in self.members]
        specs = [(str(m["path"]), int(m["sizeMiB"]) * MIB) for m in planned_members]
        backend = self.backend
        secret_holder = [password]
        password = None

        def work() -> dict[str, Any]:
            try:
                for member in planned_members:
                    folder = Path(str(member["path"])).parent
                    if member.get("hostRoot") and (folder.is_junction() or folder.is_symlink()):
                        raise RuntimeError(f"PoolFS folder is a junction or link: {folder}")
                    folder.mkdir(parents=True, exist_ok=True)
                    if member.get("hostRoot") and (
                        folder.is_junction() or folder.is_symlink() or
                        not self._same_mount(str(member["hostRoot"]), str(member["hostGuid"]))
                    ):
                        raise RuntimeError("A selected host folder or volume changed before formatting.")
                return backend.format_v6(specs, secret_holder[0])
            finally:
                secret_holder[0] = None

        def failed(message: str) -> None:
            created = []
            for member in self.members:
                if Path(str(member["path"])).exists():
                    member["partial"] = True
                    created.append(str(member["path"]))
            if created:
                self._refresh_members()
                warning = (
                    f"{message}\n\nFormatting stopped after one or more image paths were created. "
                    "The listed files have been left in place for inspection. "
                    "Do not mount them as a complete pool.\n\n" + "\n".join(created)
                )
                self._message("Pool creation incomplete; inspect the created image paths.")
                QMessageBox.warning(self, "Pool creation incomplete", warning)
            else:
                self._error(message)

        def completed(result: dict[str, Any]) -> None:
            for member in self.members:
                member["planned"] = False
            self.config = None
            self.config_path = None
            self.require_check.setChecked(encrypted)
            self._refresh_members()
            self._message(f"Created {name} with {len(self.members)} image(s). Save its config.")
            QMessageBox.information(
                self, "Pool created",
                "The member images were created. Save a JSON config to mount or transport this pool."
            )
            self.save_config()

        self._run_operation("Creating PoolFS images…", work, completed, failed,
                            show_progress=True)

    def grow_images(self) -> None:
        config = self._current_config()
        if config is None or self.backend is None:
            return
        sources = [Path(str(member["path"])) for member in self.members]
        if not sources or any(not source.is_file() for source in sources):
            self._error("Every member image must be present before expansion.")
            return
        dialog = QDialog(self)
        dialog.setWindowTitle("Expand PoolFS images")
        dialog.setMinimumWidth(800)
        box = QVBoxLayout(dialog)
        box.setContentsMargins(22, 20, 22, 18)
        box.addWidget(label("Expand one or more members", "section"))
        box.addWidget(label(
            "Enter the new size for each member. PoolFS creates verified copies "
            "beside the originals; every member needs a new copy because the "
            "size manifest is shared. Original images stay intact.", "muted", True
        ))
        sizes = table(["Existing member", "Current", "New size (MiB)"])
        sizes.setRowCount(len(sources))
        sizes.setMinimumHeight(min(500, 55 + len(sources) * 43))
        sizes.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        inputs: list[QLineEdit] = []
        for row, source in enumerate(sources):
            current_mib = source.stat().st_size // MIB
            self._put(sizes, row, 0, str(source))
            self._put(sizes, row, 1, f"{current_mib:,} MiB")
            field = QLineEdit(str(current_mib))
            field.setValidator(QRegularExpressionValidator(QRegularExpression(r"[0-9]{0,13}")))
            field.setAccessibleName(f"New size in MiB for {source.name}")
            sizes.setCellWidget(row, 2, field)
            sizes.setRowHeight(row, 43)
            inputs.append(field)
        box.addWidget(sizes)
        controls = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        controls.accepted.connect(dialog.accept)
        controls.rejected.connect(dialog.reject)
        box.addWidget(controls)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return
        specs: list[tuple[str, str, int]] = []
        taken = {str(source).casefold() for source in sources}
        growing = False
        requested_by_host: dict[str, tuple[str, int]] = {}
        try:
            for source, field in zip(sources, inputs):
                current_mib = source.stat().st_size // MIB
                requested_mib = int(field.text())
                if not current_mib * MIB == source.stat().st_size:
                    raise ValueError(f"Member length is not MiB-aligned: {source}")
                if not current_mib <= requested_mib <= MAX_IMAGE_MIB:
                    raise ValueError(f"New size for {source.name} must be at least {current_mib:,} MiB.")
                growing |= requested_mib > current_mib
                for number in range(1, 10001):
                    suffix = "-grown" if number == 1 else f"-grown-{number}"
                    destination = source.with_name(f"{source.stem}{suffix}{source.suffix}")
                    if not destination.exists() and str(destination).casefold() not in taken:
                        break
                else:
                    raise ValueError(f"Could not find a new image name beside {source}.")
                taken.add(str(destination).casefold())
                root, key, filesystem = self._member_host({"path": str(destination)})
                if filesystem.upper() == "FAT32" and requested_mib > 0xFFFFFFFF // MIB:
                    raise ValueError(f"FAT32 cannot hold the requested copy of {source.name}.")
                previous = requested_by_host.get(key, (root, 0))[1]
                requested_by_host[key] = (root, previous + requested_mib)
                specs.append((str(source), str(destination), requested_mib * MIB))
            if not growing:
                raise ValueError("Increase the size of at least one member.")
            for root, requested_mib in requested_by_host.values():
                if shutil.disk_usage(root).free // MIB < requested_mib:
                    raise ValueError(
                        f"Not enough space on {root} for the new copies "
                        f"({requested_mib:,} MiB needed)."
                    )
            old_config = Path(self.config_path)
            for number in range(1, 10001):
                suffix = "-grown" if number == 1 else f"-grown-{number}"
                new_config = old_config.with_name(f"{old_config.stem}{suffix}.json")
                if not new_config.exists():
                    break
            else:
                raise ValueError("Could not find an unused config name.")
        except (OSError, ValueError) as exc:
            self._error(str(exc))
            return
        total_mib = sum(size // MIB for _, _, size in specs)
        if QMessageBox.question(
            self, "Confirm expanded copies",
            f"Create {len(specs)} new member copies totaling {total_mib:,} MiB "
            f"and save {new_config.name}? The current images and config remain intact."
        ) != QMessageBox.StandardButton.Yes:
            return
        try:
            encrypted = bool(self.backend.config_is_encrypted(self.config_path)["encrypted"])
        except Exception as exc:
            self._error(str(exc))
            return
        password = self._password("Unlock pool to expand") if encrypted else None
        if encrypted and password is None:
            return
        backend = self.backend
        secret_holder = [password]
        password = None

        def work() -> dict[str, Any]:
            try:
                backend.grow_v6(specs, secret_holder[0])
                backend.save_config(
                    str(new_config), str(config["name"]), str(config["drive"]),
                    [destination for _, destination, _ in specs],
                    bool(config["requireEncrypted"]),
                )
                return backend.load_config(str(new_config))
            finally:
                secret_holder[0] = None

        def completed(result: dict[str, Any]) -> None:
            self.config_path = str(new_config)
            self.config = result
            self.members = [{"path": path, "planned": False}
                            for path in result["members"]]
            self._refresh_members()
            self._remember_config(str(new_config))
            self._message(f"Expanded copies ready: {new_config.name}")
            QMessageBox.information(
                self, "Expanded pool ready",
                f"The expanded images passed verification. The manager now uses "
                f"{new_config.name}. Original images and config remain intact."
            )

        def failed(message: str) -> None:
            created = [destination for _, destination, _ in specs
                       if Path(destination).exists()]
            if created:
                self._message("Expansion incomplete; original images remain intact.")
                QMessageBox.warning(
                    self, "Expansion incomplete",
                    f"{message}\n\nOriginal images and config remain intact. "
                    "Do not mount these incomplete new copies; inspect them before retrying:\n" +
                    "\n".join(created)
                )
            else:
                self._error(message)

        self._run_operation("Expanding and verifying member copies…", work, completed, failed)

    def save_config(self) -> bool:
        if self.backend is None:
            self._error("Native PoolFS bridge is unavailable.")
            return False
        if not self.members or any(m.get("planned") for m in self.members):
            self._error("Create queued image files before saving their config.")
            return False
        try:
            name = validate_pool_name(self.pool_name.text())
        except ValueError as exc:
            self._error(str(exc))
            return False
        destination, _ = QFileDialog.getSaveFileName(
            self, "Save PoolFS config", self.config_path or f"poolfs-{name}.json",
            "JSON files (*.json)"
        )
        if not destination:
            return False
        if not destination.lower().endswith(".json"):
            destination += ".json"
        if self.workspace:
            other = self.workspace.find_config(destination)
            if other is not None and other is not self:
                self._error("This config is already open for another pool. Choose another filename.")
                return False
        expected = {
            "name": name,
            "drive": self.drive_combo.currentText(),
            "requireEncrypted": self.require_check.isChecked(),
            "members": [str(m["path"]) for m in self.members],
        }
        result = self._call_sync(
            "Saving PoolFS config…",
            lambda: self.backend.save_config(
                destination, expected["name"], expected["drive"], expected["members"],
                expected["requireEncrypted"],
            ),
        )
        if result is None:
            return False
        self.config = expected
        self.config_path = destination
        self._refresh_members()
        self._remember_config(destination)
        self._message(f"Saved {Path(destination).name}")
        return True

    def _current_config(self) -> dict[str, Any] | None:
        if self.backend is None:
            self._error("Native PoolFS bridge is unavailable.")
            return None
        if not self.config_path:
            self._error("Save a JSON config before mounting or verifying.")
            return None
        current = {
            "name": self.pool_name.text(),
            "drive": self.drive_combo.currentText(),
            "requireEncrypted": self.require_check.isChecked(),
            "members": [str(m["path"]) for m in self.members],
        }
        if current != self.config:
            self._error("The config options or members changed. Save them before continuing.")
            return None
        return current

    def mount_pool(self, startup: bool = False) -> None:
        config = self._current_config()
        if config is None or self.backend is None:
            return
        encryption = self._call_sync(
            "Inspecting pool encryption…",
            lambda: self.backend.config_is_encrypted(self.config_path or ""),
        )
        if encryption is None:
            return
        password = self._password("Unlock PoolFS") if encryption.get("encrypted") else None
        if encryption.get("encrypted") and password is None:
            return
        backend = self.backend
        config_path = self.config_path or ""
        secret_holder = [password]
        password = None

        def work() -> dict[str, Any]:
            try:
                return backend.mount_config(config_path, config, secret_holder[0])
            finally:
                secret_holder[0] = None

        def completed(result: dict[str, Any]) -> None:
            self._message(f"Mounted PoolFS on {config['drive']}")
            if startup:
                QTimer.singleShot(0, self, self._quit_after_startup)

        self._run_operation("Mounting PoolFS…", work, completed)

    def _quit_after_startup(self) -> None:
        panels = self.workspace.pools() if self.workspace else [self]
        if any(panel._tasks or panel._running_operation for panel in panels):
            QTimer.singleShot(50, self, self._quit_after_startup)
            return
        QApplication.quit()

    def verify_pool(self) -> None:
        config = self._current_config()
        if config is None or self.backend is None:
            return
        encryption = self._call_sync(
            "Inspecting pool encryption…",
            lambda: self.backend.config_is_encrypted(self.config_path or ""),
        )
        if encryption is None:
            return
        password = self._password("Verify encrypted pool") if encryption.get("encrypted") else None
        if encryption.get("encrypted") and password is None:
            return
        backend = self.backend
        config_path = self.config_path or ""
        secret_holder = [password]
        password = None

        def work() -> dict[str, Any]:
            try:
                return backend.verify_config(config_path, secret_holder[0])
            finally:
                secret_holder[0] = None

        def completed(result: dict[str, Any]) -> None:
            summary = (
                f"Data issues: {result.get('issues', '?')}\n"
                f"Capacity: {human_size(result.get('capacity'))}\n"
                f"Available: {human_size(result.get('available'))}\n"
                f"Incomplete journal tail: {'yes' if result.get('incompleteTail') else 'no'}"
            )
            QMessageBox.information(self, "Verification result", summary)
            recorded = True
            if result.get("issues") == 0 and not result.get("incompleteTail"):
                try:
                    Path(config_path + ".verified").write_text(
                        datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ") + "\n",
                        encoding="ascii",
                    )
                except OSError:
                    recorded = False
            self._message("Pool verification completed." if recorded else
                          "Verification passed, but its timestamp could not be saved.")

        self._run_operation("Verifying PoolFS data…", work, completed)

    def unmount_pool(self) -> None:
        if self.backend is None:
            self._error("Native PoolFS bridge is unavailable.")
            return
        drive = self.drive_combo.currentText()
        backend = self.backend
        config = self.config

        def work() -> dict[str, Any]:
            if config is None or config["drive"] != drive or backend.match_mount(drive, config) is not True:
                raise BridgeError(f"Could not confirm that this pool is mounted on {drive}.")
            return backend.unmount(drive)

        def completed(result: dict[str, Any]) -> None:
            self._message(f"Unmounted {drive}")
            self._update_partition_buttons()

        self._run_operation(
            f"Unmounting {drive}…", work, completed
        )

    def reclaim_space(self) -> None:
        config = self._current_config()
        if config is None or self.backend is None:
            return
        drive = str(config["drive"])
        backend = self.backend

        def work() -> dict[str, Any]:
            if backend.match_mount(drive, config) is not True:
                raise BridgeError(f"Could not confirm that this pool is mounted on {drive}.")
            return backend.reclaim(drive)

        def completed(result: dict[str, Any]) -> None:
            report = str(result.get("output", "Checkpoint rotated.")).strip()
            self._message(report)
            QMessageBox.information(self, "Space reclaimed", report)
            self.refresh_inventory()

        self._run_operation(
            f"Reclaiming space on {drive}…", work, completed
        )

    def restart_as_admin(self) -> None:
        if os.name != "nt":
            self._error("Partition controls currently require Windows.")
            return
        if self.inventory.get("partitionElevated"):
            QMessageBox.information(self, "Administrator", "This manager is already elevated.")
            return
        bridge_argument = (
            f'--bridge "{Path(self.backend.path).resolve()}"'
            if self.backend is not None else ""
        )
        if getattr(sys, "frozen", False):
            executable = sys.executable
            arguments = bridge_argument or None
        else:
            executable = sys.executable
            script = Path(__file__).resolve()
            arguments = f'"{script}" {bridge_argument}'.rstrip()
        shell_execute = ctypes.windll.shell32.ShellExecuteW
        shell_execute.restype = ctypes.c_void_p
        result = shell_execute(None, "runas", executable, arguments, None, 1)
        if not result or int(result) <= 32:
            self._error("Administrator restart was cancelled or unavailable.")
        else:
            self._message("Opened a separate Administrator PoolFS Manager window.")

    def _update_partition_buttons(self) -> None:
        disk = self._selected_disk()
        part = self._selected_partition()
        elevated = bool(self.inventory.get("partitionElevated"))
        mounted = []
        if elevated and self.backend is not None:
            try:
                mounted = self.backend.mounts().get("mountedDrives", [])
            except Exception:
                mounted = ["unknown"]
        ready = elevated and not mounted
        disk_number = disk.get("number", 0) if disk else 0
        disk_style = disk.get("partitionStyle") if disk else None
        raw = ready and disk_number != 0 and disk_style == 2
        gpt = ready and disk_number != 0 and disk_style == 1
        basic = bool(part and str(part.get("type", "")).upper().startswith("{EBD0A0A2-"))
        self.partition_buttons[0].setEnabled(raw)
        self.partition_buttons[1].setEnabled(gpt)
        for action in (2, 3, 4):
            self.partition_buttons[action].setEnabled(gpt and basic)
        self.partition_notice.setText(
            "Choose Run as admin to enable partition controls. Adding images needs no elevation."
            if not elevated else
            "Unmount all PoolFS drives before changing host partitions."
            if mounted else
            "Select a disk or basic-data partition. Every change gets a preview and typed confirmation."
        )

    def _partition_options(self, request: PartitionRequest, action: int) -> bool:
        if action not in (1, 2, 4):
            return True
        dialog = QDialog(self)
        dialog.setWindowTitle(
            "Create partition" if action == 1 else
            "Quick format" if action == 2 else "Change drive letter"
        )
        dialog.setMinimumWidth(380)
        form = QFormLayout(dialog)
        size = QSpinBox()
        size.setRange(512, 16_000_000)
        size.setValue(512)
        size.setSuffix(" MiB")
        maximum = QCheckBox("Use maximum available space")
        if action == 1:
            form.addRow("Size", size)
            form.addRow("", maximum)
        letter = QComboBox()
        letter.addItems([chr(value) for value in range(ord("D"), ord("Z") + 1)])
        if action in (1, 4):
            form.addRow("New drive letter", letter)
        filesystem = QComboBox()
        filesystem.addItems(["NTFS", "exFAT", "FAT32"])
        name = QLineEdit()
        if action in (1, 2):
            form.addRow("File system", filesystem)
            form.addRow("Label", name)
        controls = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        controls.accepted.connect(dialog.accept)
        controls.rejected.connect(dialog.reject)
        form.addRow(controls)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return False
        if action == 1:
            request.size_mib = 0 if maximum.isChecked() else size.value()
        if action in (1, 4):
            request.drive_letter = letter.currentText()
        if action in (1, 2):
            request.filesystem = filesystem.currentText()
            request.label = name.text()
        return True

    def _confirm_partition(self, summary: str, phrase: str) -> bool:
        dialog = QDialog(self)
        dialog.setWindowTitle("Confirm partition change")
        dialog.setMinimumWidth(560)
        box = QVBoxLayout(dialog)
        box.setContentsMargins(22, 22, 22, 18)
        box.addWidget(label("Review the exact target", "section"))
        details = label(summary, "muted", True)
        details.setMinimumHeight(86)
        box.addWidget(details)
        box.addWidget(label(f"Type {phrase} to continue.", "muted"))
        typed = QLineEdit()
        typed.setPlaceholderText(phrase)
        box.addWidget(typed)
        controls = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        accept = controls.button(QDialogButtonBox.StandardButton.Ok)
        accept.setEnabled(False)
        typed.textChanged.connect(lambda value: accept.setEnabled(value == phrase))
        controls.accepted.connect(dialog.accept)
        controls.rejected.connect(dialog.reject)
        box.addWidget(controls)
        return dialog.exec() == QDialog.DialogCode.Accepted

    def partition_action(self, action: int) -> None:
        if self.backend is None:
            self._error("Native PoolFS bridge is unavailable.")
            return
        disk = self._selected_disk()
        part = self._selected_partition()
        if disk is None or (action in (2, 3, 4) and part is None):
            self._error("Select the exact disk or partition row first.")
            return
        request = PartitionRequest()
        request.action = action
        request.disk_number = int(disk["number"])
        request.expected_disk_bytes = int(disk.get("totalBytes") or 0)
        request.expected_disk_guid = str(disk.get("diskGuid") or "")
        request.partition_number = int(part.get("number", 0)) if part else 0
        request.expected_partition_guid = str(part.get("partitionGuid") or "") if part else ""
        request.expected_partition_offset = int(part.get("offsetBytes") or 0) if part else 0
        request.expected_partition_bytes = int(part.get("lengthBytes") or 0) if part else 0
        request.size_mib = 0
        request.drive_letter = "\0"
        request.filesystem = "NTFS"
        request.label = ""
        request.disk_token = ""
        if not self._partition_options(request, action):
            return
        phrase = f"Disk {disk['number']}"
        if action in (2, 3, 4) and part is not None:
            phrase += f" Partition {part['number']}"
        backend = self.backend

        def applied(result: dict[str, Any]) -> None:
            self.refresh_inventory()
            QMessageBox.information(self, "Partition changed", str(result.get("summary", "")))

        def apply_failed(message: str) -> None:
            self._error(message)
            self.refresh_inventory()

        def previewed(preview: dict[str, Any]) -> None:
            if not self._confirm_partition(str(preview.get("summary", "")), phrase):
                self._message("Partition change cancelled.")
                return
            request.disk_token = str(preview.get("diskToken") or "")
            self._run_operation(
                "Applying partition change…",
                lambda: backend.partition(request, True),
                applied, apply_failed,
            )

        self._run_operation(
            "Checking disk identity and partition safety…",
            lambda: backend.partition(request, False), previewed,
        )


class PoolWorkspace(QMainWindow):
    def __init__(self, backend: NativeBackend | None, auto_refresh: bool = True,
                 settings: QSettings | None = None) -> None:
        super().__init__()
        self.backend = backend
        self._sidebar_capacity: dict[str, tuple[int, int] | None] = {}
        self.settings = settings if settings is not None else QSettings(
            QSettings.Format.NativeFormat, QSettings.Scope.UserScope, "PoolFS", "Manager", self
        )
        self.setWindowTitle("PoolFS Manager")
        self.resize(1280, 860)
        self.setMinimumSize(1050, 690)
        root = QWidget()
        root.setObjectName("root")
        layout = QHBoxLayout(root)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        self.setCentralWidget(root)
        self.pool_stack = QStackedWidget()
        layout.addWidget(self._build_sidebar())
        layout.addWidget(self.pool_stack, 1)
        self.pool_stack.currentChanged.connect(self._sync_pool_selection)
        self.pool_list.currentRowChanged.connect(self._select_pool)
        first = self.add_pool()
        self.mount_timer = QTimer(self)
        self.mount_timer.timeout.connect(self.refresh_mounts)
        self.mount_timer.start(2500)
        if auto_refresh:
            first.refresh_inventory()

    def _build_sidebar(self) -> QFrame:
        rail = QFrame()
        rail.setObjectName("sidebar")
        rail.setFixedWidth(246)
        box = QVBoxLayout(rail)
        box.setContentsMargins(16, 22, 16, 16)
        box.setSpacing(8)
        brand = label("PoolFS", "title")
        box.addWidget(brand)
        box.addSpacing(7)
        quick = QHBoxLayout()
        quick.setSpacing(6)
        open_configs = button("Open configs…", lambda: self.pool_stack.currentWidget().open_config(), True)
        open_configs.setShortcut(QKeySequence(QKeySequence.StandardKey.Open))
        open_configs.setToolTip("Open one or more saved configs (Ctrl+O)")
        quick.addWidget(open_configs, 1)
        new_pool = button("+ New", self.add_pool)
        new_pool.setAccessibleName("New pool")
        new_pool.setShortcut(QKeySequence(QKeySequence.StandardKey.New))
        new_pool.setToolTip("Start a new pool draft (Ctrl+N)")
        quick.addWidget(new_pool)
        box.addLayout(quick)
        recent = QPushButton("Recent configs")
        recent_choices = recent_menu(recent)
        recent_choices.aboutToShow.connect(
            lambda: self.pool_stack.currentWidget()._populate_recent_configs(recent_choices)
        )
        box.addWidget(recent)
        box.addSpacing(12)
        pool_heading = QHBoxLayout()
        pool_heading.addWidget(label("OPEN POOLS", "micro"), 1)
        self.close_pool_button = button("Close selected", lambda: self.close_pool(self.pool_stack.currentIndex()))
        self.close_pool_button.setObjectName("nav")
        self.close_pool_button.setAccessibleName("Close selected pool")
        self.close_pool_button.setToolTip("Close this workspace entry. A mounted drive stays available.")
        pool_heading.addWidget(self.close_pool_button)
        box.addLayout(pool_heading)
        self.pool_list = QListWidget()
        self.pool_list.setObjectName("openPools")
        self.pool_list.setItemDelegate(PoolEntryDelegate(self.pool_list))
        self.pool_list.setAccessibleName("Open pools")
        self.pool_list.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.pool_list.setTextElideMode(Qt.TextElideMode.ElideRight)
        self.pool_list.setMinimumHeight(92)
        box.addWidget(self.pool_list, 1)
        box.addSpacing(12)
        box.addWidget(label("THIS COMPUTER", "micro"))
        self.machine_nav: dict[str, QPushButton] = {}
        for name, caption in (("Storage", "Disks && volumes"), ("Images", "Find images")):
            item = button(caption, lambda _checked=False, page=name:
                          self.pool_stack.currentWidget()._set_page(page))
            item.setObjectName("nav")
            item.setCheckable(True)
            box.addWidget(item)
            self.machine_nav[name] = item
        about = button("About PoolFS", lambda: self.pool_stack.currentWidget()._show_about())
        about.setObjectName("nav")
        box.addWidget(about)
        return rail

    def _select_pool(self, row: int) -> None:
        if 0 <= row < self.pool_stack.count():
            self.pool_stack.setCurrentIndex(row)

    def _sync_pool_selection(self, index: int) -> None:
        old = self.pool_list.blockSignals(True)
        self.pool_list.setCurrentRow(index)
        self.pool_list.blockSignals(old)
        panel = self.pool_stack.currentWidget()
        if panel:
            active_view = panel.pages.currentIndex()
            self.update_navigation(panel, ("Pool", "Storage", "Images", "Stats", "Health")[active_view])
            self.close_pool_button.setEnabled(bool(
                panel.config_path or panel.members or self.pool_stack.count() > 1
            ))

    def update_navigation(self, panel: ManagerWindow, page: str) -> None:
        if self.pool_stack.currentWidget() is panel:
            for name, item in self.machine_nav.items():
                item.setChecked(name == page)

    def pools(self) -> list[ManagerWindow]:
        return [self.pool_stack.widget(index) for index in range(self.pool_stack.count())]

    def add_pool(self) -> ManagerWindow:
        previous = self.pool_stack.currentWidget()
        panel = ManagerWindow(self.backend, auto_refresh=False, settings=self.settings, workspace=self)
        panel.drive_combo.setCurrentText(panel._suggest_free_drive())
        if previous is not None and previous._inventory_loaded:
            panel._on_inventory(previous.inventory)
        entry = QListWidgetItem("New pool")
        entry.setSizeHint(QSize(0, 58))
        entry.setTextAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop)
        self.pool_list.addItem(entry)
        self.pool_stack.addWidget(panel)
        self.pool_stack.setCurrentWidget(panel)
        self.refresh_mounts()
        return panel

    def reserved_drives(self, panel: ManagerWindow) -> set[str]:
        return {other.drive_combo.currentText() for other in self.pools() if other is not panel}

    def update_pool_entry(self, panel: ManagerWindow, status: str) -> None:
        index = self.pool_stack.indexOf(panel)
        if index < 0:
            return
        item = self.pool_list.item(index)
        drive = panel.drive_combo.currentText()
        if panel.config_path or panel.members:
            title = panel.pool_name.text() or "PoolFS"
        else:
            title = "New pool"
        captions = {
            "PoolFS mounted": "Mounted",
            "Different pool mounted": "Drive in use",
            "PoolFS mount unverified": "Check drive",
        }
        subtitle = captions.get(status, status) if panel.config_path else "Draft"
        text = f"{title}\n{drive}  ·  {subtitle}"
        if item.text() != text:
            item.setText(text)
            item.setForeground(QColor("#a5f3d8" if status == "PoolFS mounted" else "#d2ddec"))
        usage: tuple[int, int] | str | None = None
        if panel.config_path:
            if status == "PoolFS mounted":
                usage = self._pool_capacity(drive) or "Usage unavailable"
            else:
                usage = "Mount to see usage" if status == "Unmounted" else "Usage unavailable"
        if item.data(POOL_USAGE_ROLE) != usage:
            item.setData(POOL_USAGE_ROLE, usage)
            height = 88 if isinstance(usage, tuple) else (75 if usage else 58)
            item.setSizeHint(QSize(0, height))
        usage_text = " · ".join(pool_usage_labels(*usage)) if isinstance(usage, tuple) else usage
        detail = f"{title} · {drive} · {subtitle}"
        if usage_text:
            detail += f"\n{usage_text}"
        item.setData(Qt.ItemDataRole.AccessibleTextRole, detail)
        item.setToolTip(f"{detail}\n{panel.config_path or ''}".rstrip())
        if self.pool_stack.currentWidget() is panel:
            self.close_pool_button.setEnabled(bool(
                panel.config_path or panel.members or self.pool_stack.count() > 1
            ))

    def _pool_capacity(self, drive: str) -> tuple[int, int] | None:
        if drive not in self._sidebar_capacity:
            capacity = None
            if self.backend is not None:
                try:
                    sample = self.backend.stats(drive)
                    available, total = int(sample["availableBytes"]), int(sample["capacityBytes"])
                    if total > 0 and 0 <= available <= total:
                        capacity = (available, total)
                except (BridgeError, OSError, KeyError, TypeError, ValueError):
                    pass
            self._sidebar_capacity[drive] = capacity
        return self._sidebar_capacity[drive]

    def find_config(self, path: str) -> ManagerWindow | None:
        key = os.path.normcase(os.path.abspath(path))
        return next((panel for panel in self.pools() if panel.config_path and
                     os.path.normcase(os.path.abspath(panel.config_path)) == key), None)

    def open_pool_config(self, path: str, *, remember: bool = True) -> bool:
        panel = self.find_config(path)
        if panel is not None:
            self.pool_stack.setCurrentWidget(panel)
            if remember:
                panel._remember_config(path)
            panel._set_page("Pool")
            return True
        current = self.pool_stack.currentWidget()
        use_current = not current.config_path and not current.members and not current._running_operation
        panel = current if use_current else self.add_pool()
        if not panel.load_config_path(os.path.abspath(path), remember=remember):
            if not use_current:
                self._remove_pool(self.pool_stack.indexOf(panel))
            return False
        self.refresh_mounts()
        return True

    def open_discovered_pool(self, paths: list[str], encrypted: bool) -> None:
        current = self.pool_stack.currentWidget()
        panel = current if not current.members and not current.config_path else self.add_pool()
        panel._load_discovered_members(paths, encrypted)
        panel._set_page("Pool")
        panel._message(f"Loaded {len(paths)} member image(s). Name the pool and save a config.")

    def refresh_mounts(self) -> None:
        self._sidebar_capacity.clear()
        drives = None
        if self.backend is not None:
            try:
                drives = {str(drive).upper() for drive in self.backend.mounts()["mountedDrives"]}
            except Exception:
                pass
        for panel in self.pools():
            panel._apply_mount_state(drives)

    def operation_in_progress(self) -> bool:
        return any(panel._running_operation for panel in self.pools())

    def _remove_pool(self, index: int) -> None:
        panel = self.pool_stack.widget(index)
        old = self.pool_list.blockSignals(True)
        self.pool_stack.removeWidget(panel)
        self.pool_list.takeItem(index)
        self.pool_list.blockSignals(old)
        panel.deleteLater()
        if not self.pool_stack.count():
            self.add_pool()
        else:
            self._sync_pool_selection(self.pool_stack.currentIndex())

    def close_pool(self, index: int) -> None:
        panel = self.pool_stack.widget(index)
        if panel is None:
            return
        if panel._tasks or panel._running_operation:
            self.pool_stack.setCurrentWidget(panel)
            panel._message("Wait for the current PoolFS operation to finish before closing this pool.")
            return
        current = {
            "name": panel.pool_name.text(),
            "drive": panel.drive_combo.currentText(),
            "requireEncrypted": panel.require_check.isChecked(),
            "members": [str(member["path"]) for member in panel.members],
        }
        if panel.members and current != panel.config:
            choice = QMessageBox.question(
                self, "Close pool", "Discard the unsaved changes in this pool?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if choice != QMessageBox.StandardButton.Yes:
                return
        self._remove_pool(index)

    def closeEvent(self, event: Any) -> None:
        for panel in self.pools():
            if panel._tasks or panel._running_operation:
                event.ignore()
                self.pool_stack.setCurrentWidget(panel)
                panel._message("Wait for the current PoolFS operation to finish before closing.")
                return
        super().closeEvent(event)


def main() -> int:
    parser = argparse.ArgumentParser(description="PoolFS Qt desktop manager")
    parser.add_argument("--bridge", type=Path, help="Native PoolFS bridge DLL")
    parser.add_argument("--screenshot", type=Path, help="Save a screenshot and exit")
    parser.add_argument("--page", choices=("Pool", "Storage", "Images", "Stats", "Health"), default="Storage")
    parser.add_argument("--startup-mount", type=Path,
                        help="Mount a saved pool at sign-in, then close after success")
    parser.add_argument("configs", type=Path, nargs="*", help="Saved pool configs to open in the workspace")
    args = parser.parse_args()
    if args.startup_mount and args.configs:
        parser.error("open configs separately from --startup-mount")
    app = QApplication(sys.argv[:1])
    app.setStyle("Fusion")
    app.setStyleSheet(STYLE)
    backend: NativeBackend | None
    try:
        backend = NativeBackend(args.bridge)
    except Exception as exc:
        backend = None
        startup_error = str(exc)
    else:
        startup_error = ""
    workspace = PoolWorkspace(backend, auto_refresh=args.screenshot is None and args.startup_mount is None)
    for config in args.configs:
        workspace.open_pool_config(str(config), remember=args.screenshot is None)
    window = workspace.pool_stack.currentWidget()
    if startup_error:
        window._message(startup_error)
    if args.screenshot is not None:
        window.load_inventory_sync()
        if args.page == "Images" and backend is not None:
            try:
                window._on_images(backend.scan_images())
            except Exception as exc:
                window._message(str(exc))
                window._images_scanned = True
        window._set_page(args.page)
        workspace.show()
        def capture() -> None:
            args.screenshot.parent.mkdir(parents=True, exist_ok=True)
            if not workspace.grab().save(str(args.screenshot), "PNG"):
                raise RuntimeError(f"Could not save screenshot to {args.screenshot}")
            app.quit()
        QTimer.singleShot(180, capture)
    else:
        workspace.show()
        if args.startup_mount is not None:
            config_path = str(args.startup_mount.resolve())
            deadline = time.monotonic() + 60

            def start_when_ready() -> None:
                if not Path(config_path).is_file():
                    waiting = "Waiting for the pool config to become available…"
                else:
                    if window.config_path != config_path and not window.load_config_path(config_path, remember=False):
                        return
                    missing = [member for member in window.members
                               if not Path(str(member["path"])).is_file()]
                    if not missing:
                        window._refresh_members()
                        window.mount_pool(startup=True)
                        return
                    waiting = f"Waiting for {len(missing)} member image(s) to become available…"
                if time.monotonic() >= deadline:
                    window._error("Automatic mount timed out waiting for the config or member images.")
                    return
                window._message(waiting)
                QTimer.singleShot(2000, window, start_when_ready)

            QTimer.singleShot(0, window, start_when_ready)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
