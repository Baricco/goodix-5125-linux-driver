#!/usr/bin/env python3
"""GUI di gestione impronte per il Goodix 27c6:5125 (PySide6).

Gestisce una COLLEZIONE di impronte con nome: ne registri quante vuoi, le mappi (mosaico a
puzzle, vedi fragment_mapper), e la verifica confronta un tocco con TUTTE mostrando quale combacia.
Puoi rinominare/eliminare per gestire la collezione.

Le impronte stanno in state/fingers/<nome>.npz. Gira da utente (udev uaccess). Il matching e' SIGFM
(0 falsi positivi nei test); la verifica e' multi-frame (un tocco genuino passa se almeno un frame
aggancia).

Uso:
  tools/.venv/bin/python tools/enroll_gui.py
  tools/.venv/bin/python tools/enroll_gui.py --replay captures      # demo senza hardware
"""

import os
import sys
import re
import time
import glob
import signal
import contextlib

# silenzia il rumore di Qt su Wayland (innocuo ma invadente)
os.environ.setdefault("QT_LOGGING_RULES", "qt.qpa.wayland.textinput=false")

import numpy as np
import cv2
from PySide6 import QtCore, QtGui, QtWidgets

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from preprocessor import decode, preprocess
import fragment_mapper as FM

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FINGERS_DIR = os.path.join(ROOT, "state", "fingers")
ZOOM = 4
MAP_W, MAP_H = 540, 440
VERIFY_FRAMES = 5

DEVNULL = open(os.devnull, "w")

STYLE = """
QWidget { background:#15171c; color:#d7dae0; font-size:13px; }
QLabel#title { font-size:16px; font-weight:bold; color:#eaecef; }
QLabel#status { font-size:14px; color:#aeb4c0; }
QListWidget { background:#11131a; border:1px solid #262b34; border-radius:8px; padding:4px; }
QListWidget::item { padding:7px 8px; border-radius:6px; }
QListWidget::item:selected { background:#2d6cdf; color:white; }
QPushButton { background:#2a2f3a; border:1px solid #3a4150; border-radius:6px; padding:8px 12px; }
QPushButton:hover { background:#333a47; }
QPushButton:disabled { color:#5a606c; border-color:#262b34; }
QPushButton#primary { background:#2d6cdf; border-color:#2d6cdf; color:white; font-weight:bold; }
QPushButton#primary:hover { background:#3a7bf0; }
QPushButton#danger:checked { background:#c0433a; border-color:#c0433a; color:white; }
QLabel#map { background:#0c0e13; border:1px solid #262b34; border-radius:8px; }
"""


# --------------------------------------------------------------------- store impronte
def _safe(name):
    return re.sub(r"[^\w\-]+", "_", name.strip())[:40] or "impronta"


class FingerStore:
    def __init__(self):
        os.makedirs(FINGERS_DIR, exist_ok=True)

    def names(self):
        return sorted(os.path.splitext(os.path.basename(p))[0]
                      for p in glob.glob(os.path.join(FINGERS_DIR, "*.npz")))

    def path(self, name):
        return os.path.join(FINGERS_DIR, f"{_safe(name)}.npz")

    def exists(self, name):
        return os.path.exists(self.path(name))

    def load(self, name):
        return FM.FragmentMapper.load(self.path(name))

    def save(self, name, mapper):
        mapper.save(self.path(name))

    def delete(self, name):
        p = self.path(name)
        if os.path.exists(p):
            os.remove(p)


# --------------------------------------------------------------------- sorgenti frame
class HardwareSource:
    def __init__(self):
        import sensor
        self.s = sensor.Sensor()

    def open(self):           self.s.open()
    def capture_clear(self):  return self.s.capture_clear()
    def arm(self):            self.s.arm()
    def capture_touch(self):  return self.s.wait_touch_and_capture()
    def grab(self):           return self.s.grab()
    def lift(self):           self.s.wait_lift()
    def close(self):          self.s.close()


class ReplaySource:
    def __init__(self, dirpath):
        self.clear = decode(open(os.path.join(dirpath, "clear-0_plain.bin"), "rb").read()[8:8 + 7680])
        files = sorted(glob.glob(os.path.join(dirpath, "A_*_plain.bin")))
        self.frames = [decode(open(f, "rb").read()[8:8 + 7680]) for f in files] or [self.clear]
        self.i = 0

    def open(self):           pass
    def capture_clear(self):  return self.clear
    def arm(self):            pass
    def capture_touch(self):
        time.sleep(0.2)
        f = self.frames[self.i % len(self.frames)]; self.i += 1
        return f
    def grab(self):           return self.capture_touch()
    def lift(self):           pass
    def close(self):          pass


# --------------------------------------------------------------------- worker
class Worker(QtCore.QObject):
    clear_ready = QtCore.Signal(object)
    frame_ready = QtCore.Signal(object, str)
    failed = QtCore.Signal(str)

    def __init__(self, src):
        super().__init__()
        self.src = src
        self.stop = False

    @QtCore.Slot()
    def do_open(self):
        try:
            with contextlib.redirect_stdout(DEVNULL):
                self.src.open()
                clear = self.src.capture_clear()
            self.clear_ready.emit(clear)
        except Exception as e:
            self.failed.emit(f"apertura sensore: {e}")

    @QtCore.Slot(str)
    def do_capture(self, kind):
        if self.stop:
            return
        try:
            with contextlib.redirect_stdout(DEVNULL):
                if kind == "map":
                    f = self.src.grab()          # mappatura: cattura continua, niente danza FDT
                else:
                    self.src.arm()               # verifica: tocco singolo deliberato
                    f = self.src.capture_touch()
                    self.src.lift()
            self.frame_ready.emit(f, kind)
        except Exception as e:
            self.failed.emit(f"cattura: {e}")


# --------------------------------------------------------------------- finestra
class MainWindow(QtWidgets.QMainWindow):
    request_open = QtCore.Signal()
    request_capture = QtCore.Signal(str)

    def __init__(self, src):
        super().__init__()
        self.setWindowTitle("Goodix 5125 — Impronte")
        self.setStyleSheet(STYLE)
        self.resize(960, 600)
        self.store = FingerStore()
        self.fingers = {}          # nome -> FragmentMapper (caricate, per la verifica)
        self.session = None        # impronta attiva (in mappatura/vista)
        self.active_name = None
        self.clear = None
        self.ready = False
        self.mapping = False
        self._vleft = 0
        self._vbest = {}
        self._last_render = 0.0

        self.thread = QtCore.QThread(self)
        self.worker = Worker(src)
        self.worker.moveToThread(self.thread)
        self.worker.clear_ready.connect(self.on_clear)
        self.worker.frame_ready.connect(self.on_frame)
        self.worker.failed.connect(self.on_failed)
        self.request_open.connect(self.worker.do_open)
        self.request_capture.connect(self.worker.do_capture)
        self.thread.start()

        self._build_ui()
        self._reload_fingers()

    # ---------------- UI ----------------
    def _build_ui(self):
        # colonna sinistra: collezione + sensore
        self.list = QtWidgets.QListWidget()
        self.list.itemSelectionChanged.connect(self.on_select)
        self.btn_new = QtWidgets.QPushButton("＋ Nuova impronta", objectName="primary")
        self.btn_del = QtWidgets.QPushButton("🗑 Elimina")
        self.btn_new.clicked.connect(self.on_new)
        self.btn_del.clicked.connect(self.on_delete)
        self.btn_init = QtWidgets.QPushButton("Inizializza sensore")
        self.btn_init.clicked.connect(self.on_init)
        self.sensor_lbl = QtWidgets.QLabel("Sensore non inizializzato", objectName="status")

        left = QtWidgets.QVBoxLayout()
        left.addWidget(QtWidgets.QLabel("Impronte", objectName="title"))
        left.addWidget(self.list, 1)
        row = QtWidgets.QHBoxLayout(); row.addWidget(self.btn_new); row.addWidget(self.btn_del)
        left.addLayout(row)
        left.addSpacing(10)
        left.addWidget(self.btn_init)
        left.addWidget(self.sensor_lbl)
        left_w = QtWidgets.QWidget(); left_w.setLayout(left); left_w.setFixedWidth(240)

        # centro: mappa + azioni
        self.map_title = QtWidgets.QLabel("Nessuna impronta selezionata", objectName="title")
        # inquadratura corrente (frame live in tempo reale)
        self.live = QtWidgets.QLabel(alignment=QtCore.Qt.AlignCenter)
        self.live.setFixedSize(80 * 2, 64 * 2)
        self.live.setStyleSheet("background:#0c0e13;border:1px solid #2a2f3a;border-radius:6px;")
        self.map_label = QtWidgets.QLabel(objectName="map", alignment=QtCore.Qt.AlignCenter)
        self.map_label.setMinimumSize(MAP_W, MAP_H)
        self.btn_map = QtWidgets.QPushButton("Mappa dito", objectName="danger", checkable=True)
        self.btn_save = QtWidgets.QPushButton("Salva")
        self.btn_verify = QtWidgets.QPushButton("Verifica (tutte)", objectName="primary")
        self.btn_map.clicked.connect(self.on_toggle_map)
        self.btn_save.clicked.connect(self.on_save)
        self.btn_verify.clicked.connect(self.on_verify_start)
        self.status = QtWidgets.QLabel("Inizializza il sensore, poi crea o seleziona un'impronta.",
                                       objectName="status", wordWrap=True)

        acts = QtWidgets.QHBoxLayout()
        acts.addWidget(self.btn_map); acts.addWidget(self.btn_save)
        acts.addStretch(1); acts.addWidget(self.btn_verify)
        top = QtWidgets.QHBoxLayout()
        top.addWidget(self.map_title, 1)
        lv = QtWidgets.QVBoxLayout()
        lv.addWidget(QtWidgets.QLabel("Inquadratura", objectName="status", alignment=QtCore.Qt.AlignCenter))
        lv.addWidget(self.live)
        top.addLayout(lv)
        right = QtWidgets.QVBoxLayout()
        right.addLayout(top)
        right.addWidget(self.map_label, 1)
        right.addLayout(acts)
        right.addWidget(self.status)
        right_w = QtWidgets.QWidget(); right_w.setLayout(right)

        root = QtWidgets.QHBoxLayout()
        root.addWidget(left_w); root.addWidget(right_w, 1)
        central = QtWidgets.QWidget(); central.setLayout(root)
        self.setCentralWidget(central)
        self._sync_buttons()

    def _sync_buttons(self):
        self.btn_map.setEnabled(self.ready and self.session is not None)
        self.btn_save.setEnabled(self.session is not None and bool(self._tiles()))
        self.btn_verify.setEnabled(self.ready and len(self.fingers) > 0)
        self.btn_del.setEnabled(self.list.currentItem() is not None)

    def _tiles(self):
        return sum(len(f.kfs) for f in self.session.frags) if self.session else 0

    # ---------------- rendering ----------------
    def _rgb_pix(self, bgr):
        rgb = np.ascontiguousarray(cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB))
        qi = QtGui.QImage(rgb.data, rgb.shape[1], rgb.shape[0], rgb.shape[1] * 3,
                          QtGui.QImage.Format_RGB888)
        return QtGui.QPixmap.fromImage(qi.copy()).scaled(MAP_W, MAP_H, QtCore.Qt.KeepAspectRatio,
                                                         QtCore.Qt.SmoothTransformation)

    def _show_live(self, frame):
        """Mostra l'inquadratura corrente (ogni frame, in tempo reale)."""
        img = preprocess(frame, self.clear) if self.clear is not None else frame
        img = np.ascontiguousarray(img.astype(np.uint8))
        qi = QtGui.QImage(img.data, img.shape[1], img.shape[0], img.shape[1],
                          QtGui.QImage.Format_Grayscale8)
        self.live.setPixmap(QtGui.QPixmap.fromImage(qi.copy()).scaled(
            self.live.width(), self.live.height(), QtCore.Qt.KeepAspectRatio,
            QtCore.Qt.SmoothTransformation))

    def _show_map(self):
        if not self.session or not self.session.frags:
            self.map_label.clear()
            name = self.active_name or "—"
            self.map_title.setText(f"{name}: vuota — premi «Mappa dito»" if self.session
                                   else "Nessuna impronta selezionata")
            return
        vis, frac = self.session.render_main()
        if vis is not None:
            self.map_label.setPixmap(self._rgb_pix(vis))
        main_tiles = len(self.session.main().kfs)
        enough = "· copertura buona ✓" if (main_tiles >= 60 and frac < 0.10) else "· continua a coprire"
        self.map_title.setText(f"{self.active_name or 'senza nome'} — {main_tiles} tessere · "
                               f"buchi {frac * 100:.0f}% {enough}")

    # ---------------- collezione ----------------
    def _reload_fingers(self):
        self.fingers = {}
        for nm in self.store.names():
            try:
                self.fingers[nm] = self.store.load(nm)
            except Exception:
                pass
        self.list.blockSignals(True)
        self.list.clear()
        for nm in self.store.names():
            tiles = sum(len(f.kfs) for f in self.fingers[nm].frags) if nm in self.fingers else 0
            self.list.addItem(f"{nm}   ·   {tiles} tessere")
        self.list.blockSignals(False)
        self._sync_buttons()

    def _name_of(self, item):
        return item.text().split("   ·   ")[0] if item else None

    def on_select(self):
        nm = self._name_of(self.list.currentItem())
        if not nm or nm not in self.fingers:
            return
        self.active_name = nm
        self.session = self.fingers[nm]
        if self.ready:
            self.session.clear = self.clear      # per continuare a mappare col clear fresco
        self._show_map()
        self.status.setText(f"Impronta «{nm}» selezionata. Puoi continuare a mapparla o verificare.")
        self._sync_buttons()

    def on_new(self):
        if not self.ready:
            self.status.setText("Prima inizializza il sensore."); return
        name, ok = QtWidgets.QInputDialog.getText(self, "Nuova impronta", "Nome (es. indice destro):")
        if not ok or not name.strip():
            return
        if self.store.exists(name):
            self.status.setText(f"Esiste già un'impronta «{name}». Scegli un altro nome."); return
        self.active_name = name.strip()
        self.session = FM.FragmentMapper(self.clear)
        self.list.clearSelection()
        self._show_map()
        self.status.setText(f"«{self.active_name}»: premi «Mappa dito» e muovi il dito. Poi «Salva».")
        self._sync_buttons()

    def on_delete(self):
        nm = self._name_of(self.list.currentItem())
        if not nm:
            return
        if QtWidgets.QMessageBox.question(self, "Elimina", f"Eliminare l'impronta «{nm}»?") \
                != QtWidgets.QMessageBox.Yes:
            return
        self.store.delete(nm)
        if self.active_name == nm:
            self.session = None; self.active_name = None; self._show_map()
        self._reload_fingers()
        self.status.setText(f"Impronta «{nm}» eliminata.")

    def on_save(self):
        if not self.session or not self._tiles():
            self.status.setText("Niente da salvare: mappa prima il dito."); return
        if not self.active_name:
            name, ok = QtWidgets.QInputDialog.getText(self, "Salva impronta", "Nome:")
            if not ok or not name.strip():
                return
            self.active_name = name.strip()
        removed = self.session.prune(2)
        self.store.save(self.active_name, self.session)
        self._reload_fingers()
        # riseleziona
        for i in range(self.list.count()):
            if self._name_of(self.list.item(i)) == self.active_name:
                self.list.setCurrentRow(i); break
        self.status.setText(f"Salvata «{self.active_name}» ({self._tiles()} tessere, "
                            f"potati {removed} pezzetti).")

    # ---------------- sensore / mappatura ----------------
    def on_init(self):
        self.btn_init.setEnabled(False)
        self.sensor_lbl.setText("Inizializzo (TLS-PSK)…")
        self.request_open.emit()

    @QtCore.Slot(object)
    def on_clear(self, clear):
        self.clear = clear
        self.ready = True
        if self.session is not None:
            self.session.clear = clear
        self.sensor_lbl.setText("Sensore pronto ✓")
        self.status.setText("Pronto. Crea una «Nuova impronta» o seleziona e verifica.")
        self._sync_buttons()

    def _capture(self, kind):
        if self.ready:
            self.request_capture.emit(kind)

    def on_toggle_map(self):
        if not (self.ready and self.session is not None):
            self.btn_map.setChecked(False); return
        self.mapping = self.btn_map.isChecked()
        if self.mapping:
            self.btn_map.setText("Ferma"); self.btn_verify.setEnabled(False)
            self._capture("map")
        else:
            self.btn_map.setText("Mappa dito"); self._show_map(); self._sync_buttons()

    def on_verify_start(self):
        if not self.fingers:
            self.status.setText("Nessuna impronta salvata da verificare."); return
        self._vleft = VERIFY_FRAMES
        self._vbest = {nm: 0 for nm in self.fingers}
        self.btn_verify.setEnabled(False); self.btn_map.setEnabled(False)
        self.status.setText("Verifica: appoggia il dito e TIENILO FERMO un secondo…")
        self._capture("verify")

    @QtCore.Slot(object, str)
    def on_frame(self, frame, kind):
        self._show_live(frame)          # inquadratura corrente in tempo reale, ogni frame
        if kind == "map":
            r = self.session.add_frame(frame)
            now = time.monotonic()
            # ridisegna la mappa (throttled ~8/s) a ogni frame AGGANCIATO, cosi' il marker "sei qui"
            # segue l'inquadratura anche su zone gia' mappate; sui frame vuoti non muove nulla.
            if not r["status"].startswith("vuoto") and now - self._last_render > 0.12:
                self._show_map(); self._last_render = now
            tag = {"mappa estesa": "✓", "inizio mappa": "✓", "nuovo pezzo": "+",
                   "già coperto qui": "•", "debole": "·"}.get(
                r["status"], "↔" if r["status"].startswith("ponte") else "·")
            self.status.setText(f"{tag} {r['status']} — {r['frags']} pezzi")
            if self.mapping:
                self._capture("map")
        else:  # verify contro tutte
            import sigfm
            feats = sigfm.features(preprocess(frame, self.clear))
            for nm, fm in self.fingers.items():
                self._vbest[nm] = max(self._vbest[nm], fm.best_score(feats))
            self._vleft -= 1
            self.status.setText(f"Verifica… {VERIFY_FRAMES - self._vleft}/{VERIFY_FRAMES}")
            if self._vleft > 0:
                self._capture("verify")
            else:
                self._finish_verify()

    def _finish_verify(self):
        ranking = sorted(self._vbest.items(), key=lambda kv: -kv[1])
        winner, score = ranking[0]
        ok = score >= FM.VERIFY_MIN
        detail = "   ".join(f"{nm}:{sc}" for nm, sc in ranking[:5])
        if ok:
            self.status.setText(f"✓ RICONOSCIUTA: «{winner}»  (score {score})   —   {detail}")
        else:
            self.status.setText(f"✗ nessuna corrispondenza (miglior {winner}:{score} < {FM.VERIFY_MIN})   —   {detail}")
        self._sync_buttons()

    @QtCore.Slot(str)
    def on_failed(self, msg):
        self.sensor_lbl.setText(f"⚠ {msg}")
        self.btn_init.setEnabled(True)
        if self.mapping:
            self.mapping = False; self.btn_map.setChecked(False); self.btn_map.setText("Mappa dito")
        self._sync_buttons()

    def closeEvent(self, ev):
        self.mapping = False
        self.worker.stop = True
        self.thread.quit()
        if not self.thread.wait(3000):
            # worker bloccato in una chiamata libusb (sensore impiantato): uscita forzata
            # pulita invece di lasciare che Qt distrugga il QThread -> SIGABRT.
            os._exit(0)
        # thread terminato: nessun accesso concorrente -> teardown pulito del sensore
        try:
            if self.ready:
                self.worker.src.close()
        except Exception:
            pass
        super().closeEvent(ev)


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--replay", metavar="DIR", help="rilegge i *_plain.bin da DIR invece dell'hardware")
    args = ap.parse_args()
    src = ReplaySource(args.replay) if args.replay else HardwareSource()
    app = QtWidgets.QApplication(sys.argv)
    w = MainWindow(src); w.show()
    signal.signal(signal.SIGINT, lambda *a: w.close())
    t = QtCore.QTimer(); t.start(250); t.timeout.connect(lambda: None)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
