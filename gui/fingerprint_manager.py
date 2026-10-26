#!/usr/bin/env python3
"""Fingerprint manager — graphical front end for fprintd.

Enrolls, lists, deletes and tests a user's fingerprints through the fprintd
system service (net.reactivated.Fprint) via QtDBus. No dependency besides
PySide6: QtDBus talks to the system bus directly.

Backend = goodixfpd, which answers as fprintd (see goodixfpd/). The GUI does
NOT touch the sensor directly: it delegates everything to fprintd, so it lives
alongside fingerprint login (pam_fprintd) without fighting over the device.

Usage:  python gui/fingerprint_manager.py        (needs PySide6: pacman -S pyside6)
"""
from __future__ import annotations

import getpass
import sys

from PySide6.QtCore import Qt, QTimer, Slot, SLOT
from PySide6.QtDBus import (
    QDBusConnection, QDBusInterface, QDBusMessage, QDBusObjectPath,
    QDBusVariant,
)
from PySide6.QtWidgets import (
    QApplication, QComboBox, QFrame, QHBoxLayout, QLabel, QLineEdit,
    QListWidget, QListWidgetItem, QMainWindow, QMessageBox, QProgressBar,
    QPushButton, QVBoxLayout, QWidget,
)

FPRINT_SERVICE = "net.reactivated.Fprint"
MANAGER_PATH = "/net/reactivated/Fprint/Manager"
MANAGER_IFACE = "net.reactivated.Fprint.Manager"
DEVICE_IFACE = "net.reactivated.Fprint.Device"
PROPS_IFACE = "org.freedesktop.DBus.Properties"

# Finger slots known to fprintd -> readable label.
FINGERS = {
    "right-thumb": "Right thumb",
    "right-index-finger": "Right index finger",
    "right-middle-finger": "Right middle finger",
    "right-ring-finger": "Right ring finger",
    "right-little-finger": "Right little finger",
    "left-thumb": "Left thumb",
    "left-index-finger": "Left index finger",
    "left-middle-finger": "Left middle finger",
    "left-ring-finger": "Left ring finger",
    "left-little-finger": "Left little finger",
}

# enroll/verify status messages -> guidance text.
STATUS_HINTS = {
    "enroll-stage-passed": "Touch captured ✓ — lift the finger and place it on a slightly different spot.",
    "enroll-retry-scan": "Invalid touch — press more firmly and try again.",
    "enroll-swipe-too-short": "Too short — keep the finger still until it confirms.",
    "enroll-finger-not-centered": "Finger not centred — move it towards the centre of the sensor.",
    "enroll-remove-and-retry": "Lift the finger and place it again.",
    "enroll-data-full": "Template full.",
    "enroll-completed": "Enrollment completed ✓",
    "enroll-failed": "Enrollment failed.",
    "verify-match": "RECOGNIZED ✓",
    "verify-no-match": "Not recognized ✗ — try again touching the enrolled area.",
    "verify-retry-scan": "Invalid touch — press again.",
    "verify-swipe-too-short": "Too short — keep it still.",
    "verify-finger-not-centered": "Finger not centred.",
    "verify-remove-and-retry": "Lift and place it again.",
    "verify-unknown-error": "Unknown error.",
}


class FingerprintManager(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Fingerprint manager")
        self.resize(560, 460)

        self.bus = QDBusConnection.systemBus()
        self.dev_path: str | None = None
        self.claimed = False
        self.busy_op: str | None = None       # 'enroll' | 'verify' | None
        self.enroll_total = 0
        self.enroll_done = 0

        self._build_ui()
        self._find_device()
        self.refresh_list()

    # ---------------- UI ----------------
    def _build_ui(self) -> None:
        central = QWidget()
        root = QVBoxLayout(central)

        self.dev_label = QLabel("Sensor: —")
        self.dev_label.setStyleSheet("font-weight: bold;")
        root.addWidget(self.dev_label)

        # User row
        urow = QHBoxLayout()
        urow.addWidget(QLabel("User:"))
        self.user_edit = QLineEdit(getpass.getuser())
        urow.addWidget(self.user_edit)
        btn_load = QPushButton("Load")
        btn_load.clicked.connect(self.refresh_list)
        urow.addWidget(btn_load)
        root.addLayout(urow)

        line = QFrame(); line.setFrameShape(QFrame.HLine)
        root.addWidget(line)

        body = QHBoxLayout()

        # Left column: enrolled fingerprints
        left = QVBoxLayout()
        left.addWidget(QLabel("Enrolled fingerprints"))
        self.finger_list = QListWidget()
        left.addWidget(self.finger_list, 1)
        brow = QHBoxLayout()
        self.btn_delete = QPushButton("Delete")
        self.btn_delete.clicked.connect(self.delete_selected)
        self.btn_delete_all = QPushButton("Delete all")
        self.btn_delete_all.clicked.connect(self.delete_all)
        brow.addWidget(self.btn_delete)
        brow.addWidget(self.btn_delete_all)
        left.addLayout(brow)
        body.addLayout(left, 1)

        # Right column: actions
        right = QVBoxLayout()
        right.addWidget(QLabel("Enroll a new fingerprint"))
        self.finger_combo = QComboBox()
        right.addWidget(self.finger_combo)
        self.btn_enroll = QPushButton("Enroll")
        self.btn_enroll.clicked.connect(self.start_enroll)
        right.addWidget(self.btn_enroll)

        self.progress = QProgressBar()
        self.progress.setTextVisible(True)
        self.progress.setVisible(False)      # only during an enrollment
        right.addWidget(self.progress)

        right.addSpacing(10)
        self.btn_verify = QPushButton("Test (verify)")
        self.btn_verify.clicked.connect(self.start_verify)
        right.addWidget(self.btn_verify)

        self.btn_cancel = QPushButton("Cancel")
        self.btn_cancel.clicked.connect(self.cancel_op)
        self.btn_cancel.setEnabled(False)
        right.addWidget(self.btn_cancel)

        right.addStretch(1)
        body.addLayout(right, 1)
        root.addLayout(body, 1)

        self.status = QLabel("Ready.")
        self.status.setWordWrap(True)
        self.status.setStyleSheet("padding: 6px; background: palette(midlight); border-radius: 4px;")
        root.addWidget(self.status)

        self.setCentralWidget(central)

    # ---------------- fprintd plumbing ----------------
    def _manager(self) -> QDBusInterface:
        return QDBusInterface(FPRINT_SERVICE, MANAGER_PATH, MANAGER_IFACE, self.bus)

    def _device(self) -> QDBusInterface:
        return QDBusInterface(FPRINT_SERVICE, self.dev_path, DEVICE_IFACE, self.bus)

    def _get_prop(self, name: str):
        props = QDBusInterface(FPRINT_SERVICE, self.dev_path, PROPS_IFACE, self.bus)
        reply = props.call("Get", DEVICE_IFACE, name)
        if reply.type() == QDBusMessage.ErrorMessage or not reply.arguments():
            return None
        val = reply.arguments()[0]
        return val.variant() if isinstance(val, QDBusVariant) else val

    def _find_device(self) -> None:
        reply = self._manager().call("GetDefaultDevice")
        if reply.type() == QDBusMessage.ErrorMessage or not reply.arguments():
            self.dev_label.setText("Sensor: NOT found (is fprintd running?)")
            self._set_enabled(False)
            return
        arg = reply.arguments()[0]
        self.dev_path = arg.path() if isinstance(arg, QDBusObjectPath) else str(arg)
        if not self.dev_path.startswith("/"):
            self.dev_label.setText("Sensor: invalid path")
            self._set_enabled(False)
            self.dev_path = None
            return
        name = self._get_prop("name")
        self.dev_label.setText(f"Sensor: {name or self.dev_path}")
        # num-enroll-stages for the progress bar (via Properties.Get).
        self.enroll_total = int(self._get_prop("num-enroll-stages") or 0)
        # Connect the signals only once.
        self.bus.connect(FPRINT_SERVICE, self.dev_path, DEVICE_IFACE,
                         "EnrollStatus", self, SLOT("on_enroll_status(QString,bool)"))
        self.bus.connect(FPRINT_SERVICE, self.dev_path, DEVICE_IFACE,
                         "VerifyStatus", self, SLOT("on_verify_status(QString,bool)"))

    def _claim(self) -> bool:
        if self.claimed:
            return True
        reply = self._device().call("Claim", self.user_edit.text().strip())
        if reply.type() == QDBusMessage.ErrorMessage:
            self._error("Claim", reply.errorMessage())
            return False
        self.claimed = True
        return True

    def _release(self) -> None:
        if self.claimed:
            self._device().call("Release")
            self.claimed = False

    # ---------------- Actions ----------------
    @Slot()
    def refresh_list(self) -> None:
        if not self.dev_path:
            return
        user = self.user_edit.text().strip()
        if not self._claim():
            return
        reply = self._device().call("ListEnrolledFingers", user)
        self._release()
        self.finger_list.clear()
        enrolled: list[str] = []
        if reply.type() != QDBusMessage.ErrorMessage and reply.arguments():
            enrolled = [str(f) for f in (reply.arguments()[0] or [])]
        for slot in enrolled:
            label = FINGERS.get(slot, slot)
            item = QListWidgetItem(f"{label}  ({slot})")
            item.setData(Qt.UserRole, slot)
            self.finger_list.addItem(item)
        # Fill the combo with the fingers NOT enrolled yet.
        self.finger_combo.clear()
        for slot, label in FINGERS.items():
            if slot not in enrolled:
                self.finger_combo.addItem(label, slot)
        self._set_status(f"{len(enrolled)} fingerprints for '{user}'.")

    @Slot()
    def start_enroll(self) -> None:
        if self.busy_op or not self.dev_path:
            return
        if self.finger_combo.count() == 0:
            self._set_status("All fingers are already enrolled.")
            return
        slot = self.finger_combo.currentData()
        if not self._claim():
            return
        reply = self._device().call("EnrollStart", slot)
        if reply.type() == QDBusMessage.ErrorMessage:
            self._error("EnrollStart", reply.errorMessage())
            self._release()
            return
        self.busy_op = "enroll"
        self.enroll_done = 0
        # The number of stages can change (the driver ends early once coverage
        # converges): read it again on every enrollment.
        self.enroll_total = int(self._get_prop("num-enroll-stages") or self.enroll_total)
        self.progress.setMaximum(max(self.enroll_total, 1))
        self.progress.setValue(0)
        self.progress.setVisible(True)
        self._set_busy_ui(True)
        self._set_status("Place your finger on the sensor and follow the instructions…")

    @Slot()
    def start_verify(self) -> None:
        if self.busy_op or not self.dev_path:
            return
        # Verify against the selected finger, or 'any' if nothing is selected.
        item = self.finger_list.currentItem()
        finger = item.data(Qt.UserRole) if item else "any"
        if not self._claim():
            return
        reply = self._device().call("VerifyStart", finger)
        if reply.type() == QDBusMessage.ErrorMessage:
            self._error("VerifyStart", reply.errorMessage())
            self._release()
            return
        self.busy_op = "verify"
        self._set_busy_ui(True)
        self._set_status("Place your finger to verify…")

    @Slot()
    def cancel_op(self) -> None:
        if self.busy_op == "enroll":
            self._device().call("EnrollStop")
        elif self.busy_op == "verify":
            self._device().call("VerifyStop")
        self._finish_op("Cancelled.")

    @Slot()
    def delete_selected(self) -> None:
        item = self.finger_list.currentItem()
        if not item or self.busy_op:
            return
        slot = item.data(Qt.UserRole)
        if not self._claim():
            return
        self._device().call("DeleteEnrolledFinger", slot)
        self._release()
        self.refresh_list()

    @Slot()
    def delete_all(self) -> None:
        if self.busy_op:
            return
        if QMessageBox.question(self, "Confirm",
                                "Delete ALL fingerprints of this user?") \
                != QMessageBox.Yes:
            return
        if not self._claim():
            return
        self._device().call("DeleteEnrolledFingers", self.user_edit.text().strip())
        self._release()
        self.refresh_list()

    # ---------------- fprintd signals ----------------
    @Slot(str, bool)
    def on_enroll_status(self, result: str, done: bool) -> None:
        if self.busy_op != "enroll":
            return
        if result == "enroll-stage-passed":
            self.enroll_done += 1
            self.progress.setValue(self.enroll_done)
        hint = STATUS_HINTS.get(result, result)
        self._set_status(f"[{self.enroll_done}/{self.progress.maximum()}] {hint}")
        if done:
            ok = result == "enroll-completed"
            if ok:
                # The last stage arrives as enroll-completed (not stage-passed),
                # and when coverage converges the total is never reached.
                self.progress.setValue(self.progress.maximum())
            self._device().call("EnrollStop")
            self._finish_op("Fingerprint enrolled ✓" if ok else f"Enroll: {hint}")
            self.refresh_list()

    @Slot(str, bool)
    def on_verify_status(self, result: str, done: bool) -> None:
        if self.busy_op != "verify":
            return
        hint = STATUS_HINTS.get(result, result)
        self._set_status(hint)
        if done:
            self._device().call("VerifyStop")
            self._finish_op(hint)

    # ---------------- UI helpers ----------------
    def _finish_op(self, msg: str) -> None:
        was_enroll = self.busy_op == "enroll"
        self.busy_op = None
        self._release()
        self._set_busy_ui(False)
        self._set_status(msg)
        if was_enroll:
            # Show 100% for a moment, then remove the bar.
            QTimer.singleShot(1500, self._hide_progress)

    def _hide_progress(self) -> None:
        if self.busy_op != "enroll":
            self.progress.setVisible(False)
            self.progress.setValue(0)

    def _set_busy_ui(self, busy: bool) -> None:
        self.btn_enroll.setEnabled(not busy)
        self.btn_verify.setEnabled(not busy)
        self.btn_delete.setEnabled(not busy)
        self.btn_delete_all.setEnabled(not busy)
        self.btn_cancel.setEnabled(busy)
        self.finger_combo.setEnabled(not busy)

    def _set_enabled(self, on: bool) -> None:
        for w in (self.btn_enroll, self.btn_verify, self.btn_delete,
                  self.btn_delete_all):
            w.setEnabled(on)

    def _set_status(self, text: str) -> None:
        self.status.setText(text)

    def _error(self, where: str, msg: str) -> None:
        self._set_status(f"Error {where}: {msg}")

    def closeEvent(self, event) -> None:  # noqa: N802
        try:
            if self.busy_op == "enroll":
                self._device().call("EnrollStop")
            elif self.busy_op == "verify":
                self._device().call("VerifyStop")
            self._release()
        except Exception:
            pass
        super().closeEvent(event)


def main() -> int:
    app = QApplication(sys.argv)
    win = FingerprintManager()
    win.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
