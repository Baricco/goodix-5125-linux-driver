#!/usr/bin/env python3
"""Gestore impronte digitali — front-end grafico per fprintd.

Registra, elenca, cancella e testa le impronte di un utente tramite il
servizio di sistema fprintd (net.reactivated.Fprint) via QtDBus. Nessuna
dipendenza oltre PySide6: QtDBus parla direttamente col bus di sistema.

Backend = fprintd + libfprint goodix (Strada C, pubblicabile). La GUI NON
tocca il sensore direttamente: delega tutto a fprintd, quindi convive col
login a impronta (pam_fprintd) senza litigare il device.

Uso:  tools/.venv/bin/python tools/fingerprint_manager.py
"""
from __future__ import annotations

import getpass
import sys

from PySide6.QtCore import Qt, Slot, SLOT
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

# Slot di dito riconosciuti da fprintd -> etichetta leggibile.
FINGERS = {
    "right-thumb": "Pollice destro",
    "right-index-finger": "Indice destro",
    "right-middle-finger": "Medio destro",
    "right-ring-finger": "Anulare destro",
    "right-little-finger": "Mignolo destro",
    "left-thumb": "Pollice sinistro",
    "left-index-finger": "Indice sinistro",
    "left-middle-finger": "Medio sinistro",
    "left-ring-finger": "Anulare sinistro",
    "left-little-finger": "Mignolo sinistro",
}

# Messaggi di stato enroll/verify -> testo guida in italiano.
STATUS_HINTS = {
    "enroll-stage-passed": "Tocco acquisito ✓ — stacca il dito e riappoggia su una zona poco diversa.",
    "enroll-retry-scan": "Tocco non valido — premi più deciso e riprova.",
    "enroll-swipe-too-short": "Troppo breve — tieni il dito fermo finché non conferma.",
    "enroll-finger-not-centered": "Dito non centrato — spostalo verso il centro del sensore.",
    "enroll-remove-and-retry": "Stacca il dito e riappoggialo.",
    "enroll-data-full": "Template pieno.",
    "enroll-completed": "Registrazione completata ✓",
    "enroll-failed": "Registrazione fallita.",
    "verify-match": "RICONOSCIUTA ✓",
    "verify-no-match": "Non riconosciuta ✗ — riprova toccando la zona registrata.",
    "verify-retry-scan": "Tocco non valido — ripremi.",
    "verify-swipe-too-short": "Troppo breve — tieni fermo.",
    "verify-finger-not-centered": "Dito non centrato.",
    "verify-remove-and-retry": "Stacca e riappoggia.",
    "verify-unknown-error": "Errore sconosciuto.",
}


class FingerprintManager(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Gestore impronte")
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

        self.dev_label = QLabel("Sensore: —")
        self.dev_label.setStyleSheet("font-weight: bold;")
        root.addWidget(self.dev_label)

        # Riga utente
        urow = QHBoxLayout()
        urow.addWidget(QLabel("Utente:"))
        self.user_edit = QLineEdit(getpass.getuser())
        urow.addWidget(self.user_edit)
        btn_load = QPushButton("Carica")
        btn_load.clicked.connect(self.refresh_list)
        urow.addWidget(btn_load)
        root.addLayout(urow)

        line = QFrame(); line.setFrameShape(QFrame.HLine)
        root.addWidget(line)

        body = QHBoxLayout()

        # Colonna sinistra: impronte registrate
        left = QVBoxLayout()
        left.addWidget(QLabel("Impronte registrate"))
        self.finger_list = QListWidget()
        left.addWidget(self.finger_list, 1)
        brow = QHBoxLayout()
        self.btn_delete = QPushButton("Elimina")
        self.btn_delete.clicked.connect(self.delete_selected)
        self.btn_delete_all = QPushButton("Elimina tutte")
        self.btn_delete_all.clicked.connect(self.delete_all)
        brow.addWidget(self.btn_delete)
        brow.addWidget(self.btn_delete_all)
        left.addLayout(brow)
        body.addLayout(left, 1)

        # Colonna destra: azioni
        right = QVBoxLayout()
        right.addWidget(QLabel("Registra nuova impronta"))
        self.finger_combo = QComboBox()
        right.addWidget(self.finger_combo)
        self.btn_enroll = QPushButton("Registra (enroll)")
        self.btn_enroll.clicked.connect(self.start_enroll)
        right.addWidget(self.btn_enroll)

        self.progress = QProgressBar()
        self.progress.setTextVisible(True)
        right.addWidget(self.progress)

        right.addSpacing(10)
        self.btn_verify = QPushButton("Prova (verifica)")
        self.btn_verify.clicked.connect(self.start_verify)
        right.addWidget(self.btn_verify)

        self.btn_cancel = QPushButton("Annulla")
        self.btn_cancel.clicked.connect(self.cancel_op)
        self.btn_cancel.setEnabled(False)
        right.addWidget(self.btn_cancel)

        right.addStretch(1)
        body.addLayout(right, 1)
        root.addLayout(body, 1)

        self.status = QLabel("Pronto.")
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
            self.dev_label.setText("Sensore: NON trovato (fprintd attivo?)")
            self._set_enabled(False)
            return
        arg = reply.arguments()[0]
        self.dev_path = arg.path() if isinstance(arg, QDBusObjectPath) else str(arg)
        if not self.dev_path.startswith("/"):
            self.dev_label.setText("Sensore: path non valido")
            self._set_enabled(False)
            self.dev_path = None
            return
        name = self._get_prop("name")
        self.dev_label.setText(f"Sensore: {name or self.dev_path}")
        # num-enroll-stages per la progress bar (via Properties.Get).
        self.enroll_total = int(self._get_prop("num-enroll-stages") or 0)
        # Collega i segnali una sola volta.
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

    # ---------------- Azioni ----------------
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
        # Popola la combo coi diti NON ancora registrati.
        self.finger_combo.clear()
        for slot, label in FINGERS.items():
            if slot not in enrolled:
                self.finger_combo.addItem(label, slot)
        self._set_status(f"{len(enrolled)} impronte per '{user}'.")

    @Slot()
    def start_enroll(self) -> None:
        if self.busy_op or not self.dev_path:
            return
        if self.finger_combo.count() == 0:
            self._set_status("Tutti i diti sono già registrati.")
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
        self.progress.setMaximum(max(self.enroll_total, 1))
        self.progress.setValue(0)
        self._set_busy_ui(True)
        self._set_status("Appoggia il dito sul sensore e segui le indicazioni…")

    @Slot()
    def start_verify(self) -> None:
        if self.busy_op or not self.dev_path:
            return
        # Verifica contro un dito selezionato, o 'any' se nessuna selezione.
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
        self._set_status("Appoggia il dito per la verifica…")

    @Slot()
    def cancel_op(self) -> None:
        if self.busy_op == "enroll":
            self._device().call("EnrollStop")
        elif self.busy_op == "verify":
            self._device().call("VerifyStop")
        self._finish_op("Annullato.")

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
        if QMessageBox.question(self, "Conferma",
                                "Eliminare TUTTE le impronte di questo utente?") \
                != QMessageBox.Yes:
            return
        if not self._claim():
            return
        self._device().call("DeleteEnrolledFingers", self.user_edit.text().strip())
        self._release()
        self.refresh_list()

    # ---------------- Segnali fprintd ----------------
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
            self._device().call("EnrollStop")
            self._finish_op("Impronta registrata ✓" if ok else f"Enroll: {hint}")
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

    # ---------------- Helper UI ----------------
    def _finish_op(self, msg: str) -> None:
        self.busy_op = None
        self._release()
        self._set_busy_ui(False)
        self._set_status(msg)

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
        self._set_status(f"Errore {where}: {msg}")

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
