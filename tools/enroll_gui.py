#!/usr/bin/env python3
"""GUI di mappatura a MOSAICO per il Goodix 27c6:5125 (PySide6).

Durante la mappatura muovi lentamente il dito: ogni scansione viene cucita sulla mappa (mosaico
live). Se un frame non si aggancia ("contatto perso") sei andato troppo veloce o hai saltato:
torna dove eri. I BUCHI nella mappa sono evidenziati in rosso -> li riempi muovendo il dito li'.
La verifica usa lo score SIGFM (0 falsi positivi nei test) piu' gli inlier di registrazione.

Modello e geometria in tools/mosaic_session.py e tools/mosaic.py; vedi docs/matching.md.

Uso:
  tools/.venv/bin/python tools/enroll_gui.py
  tools/.venv/bin/python tools/enroll_gui.py --replay captures      # demo senza hardware
"""

import os
import sys
import glob
import time
import signal
import contextlib
import datetime

import numpy as np
import cv2
from PySide6 import QtCore, QtGui, QtWidgets

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from preprocessor import decode, preprocess
import fragment_mapper as FM

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATE_DIR = os.path.join(ROOT, "state")
LOG_DIR = os.path.join(ROOT, "captures", "verify_log")
ZOOM = 4
MAP_W, MAP_H = 560, 460
VERIFY_FRAMES = 5         # verifica a raffica: un tocco genuino passa se ALMENO un frame aggancia

DEVNULL = open(os.devnull, "w")

STYLE = """
QWidget { background:#15171c; color:#d7dae0; font-size:13px; }
QLabel#title { font-size:16px; font-weight:bold; color:#eaecef; }
QLabel#status { font-size:14px; color:#aeb4c0; }
QPushButton { background:#2a2f3a; border:1px solid #3a4150; border-radius:6px; padding:8px 12px; }
QPushButton:hover { background:#333a47; }
QPushButton:disabled { color:#5a606c; border-color:#262b34; }
QPushButton#primary { background:#2d6cdf; border-color:#2d6cdf; color:white; font-weight:bold; }
QPushButton#primary:hover { background:#3a7bf0; }
QPushButton#danger:checked { background:#c0433a; border-color:#c0433a; color:white; }
QLabel#map { background:#0c0e13; border:1px solid #262b34; border-radius:8px; }
"""


# --------------------------------------------------------------------- sorgenti frame
class HardwareSource:
    def __init__(self):
        import sensor
        self.s = sensor.Sensor()

    def open(self):           self.s.open()
    def capture_clear(self):  return self.s.capture_clear()
    def arm(self):            self.s.arm()
    def capture_touch(self):  return self.s.wait_touch_and_capture()
    def lift(self):           self.s.wait_lift()


class ReplaySource:
    def __init__(self, dirpath):
        self.clear = decode(open(os.path.join(dirpath, "clear-0_plain.bin"), "rb").read()[8:8 + 7680])
        files = (sorted(glob.glob(os.path.join(dirpath, "A_*_plain.bin")))
                 + sorted(glob.glob(os.path.join(dirpath, "Averif_plain.bin")))
                 + sorted(glob.glob(os.path.join(dirpath, "Bimpostore_plain.bin"))))
        self.frames = [decode(open(f, "rb").read()[8:8 + 7680]) for f in files]
        self.i = 0

    def open(self):           pass
    def capture_clear(self):  return self.clear
    def arm(self):            pass
    def capture_touch(self):
        time.sleep(0.2)
        f = self.frames[self.i % len(self.frames)]; self.i += 1
        return f
    def lift(self):           pass


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
                self.src.arm()
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
        self.setWindowTitle("Goodix 5125 — Mappatura a mosaico")
        self.setStyleSheet(STYLE)
        self.resize(940, 600)
        self.session = None
        self.clear = None
        self.mapping = False
        self.ready = False
        self._vscores, self._vframes, self._vleft = [], [], 0

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
        self._autoload()

    def _build_ui(self):
        self.live = QtWidgets.QLabel(alignment=QtCore.Qt.AlignCenter)
        self.live.setFixedSize(80 * ZOOM, 64 * ZOOM)
        self.live.setStyleSheet("background:#0c0e13;border:1px solid #2a2f3a;border-radius:8px;")
        self.status = QtWidgets.QLabel("Premi «Inizializza sensore».", objectName="status", wordWrap=True)

        self.btn_init = QtWidgets.QPushButton("Inizializza sensore", objectName="primary")
        self.btn_map = QtWidgets.QPushButton("Mappa dito", objectName="danger", checkable=True)
        self.btn_verify = QtWidgets.QPushButton("Verifica tocco")
        self.btn_save = QtWidgets.QPushButton("Salva mappa")
        for b in (self.btn_map, self.btn_verify, self.btn_save):
            b.setEnabled(False)
        self.btn_init.clicked.connect(self.on_init)
        self.btn_map.clicked.connect(self.on_toggle_map)
        self.btn_verify.clicked.connect(self.on_verify_start)
        self.btn_save.clicked.connect(self.on_save)

        left = QtWidgets.QVBoxLayout()
        left.addWidget(QtWidgets.QLabel("Tocco corrente", objectName="title"))
        left.addWidget(self.live)
        left.addSpacing(8)
        left.addWidget(self.btn_init)
        left.addWidget(self.btn_map)
        left.addWidget(self.btn_verify)
        left.addWidget(self.btn_save)
        left.addStretch(1)
        left.addWidget(self.status)
        left_w = QtWidgets.QWidget(); left_w.setLayout(left); left_w.setFixedWidth(80 * ZOOM + 24)

        self.map_title = QtWidgets.QLabel("Mappa del dito — 0 tessere", objectName="title")
        self.map_label = QtWidgets.QLabel(objectName="map", alignment=QtCore.Qt.AlignCenter)
        self.map_label.setMinimumSize(MAP_W, MAP_H)
        self.hint = QtWidgets.QLabel(
            "Mappa soprattutto la ZONA che premi di solito (centro del polpastrello), stesso "
            "orientamento: ~60 tessere nel pezzo principale bastano, i bordi sono inutili per la "
            "verifica. Muovi LENTO e continuo; rosso = buchi, cornice gialla = «dove sei». "
            "I pezzi sotto si fondono quando ci ripassi sopra facendoli combaciare.",
            objectName="status", wordWrap=True)
        # strip dei pezzi da incastrare (puzzle)
        self.pieces_host = QtWidgets.QWidget()
        self.pieces_l = QtWidgets.QHBoxLayout(self.pieces_host)
        self.pieces_l.setAlignment(QtCore.Qt.AlignLeft); self.pieces_l.setSpacing(4)
        pieces_scroll = QtWidgets.QScrollArea(); pieces_scroll.setWidgetResizable(True)
        pieces_scroll.setWidget(self.pieces_host); pieces_scroll.setFixedHeight(64 * 2 + 20)
        pieces_scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAsNeeded)

        right = QtWidgets.QVBoxLayout()
        right.addWidget(self.map_title)
        right.addWidget(self.map_label, 1)
        right.addWidget(QtWidgets.QLabel("Pezzi da incastrare:", objectName="status"))
        right.addWidget(pieces_scroll)
        right.addWidget(self.hint)
        right_w = QtWidgets.QWidget(); right_w.setLayout(right)

        root = QtWidgets.QHBoxLayout()
        root.addWidget(left_w)
        root.addWidget(right_w, 1)
        central = QtWidgets.QWidget(); central.setLayout(root)
        self.setCentralWidget(central)

    # ---- rendering ----
    def _gray_pix(self, img_u8, w, h):
        img = np.ascontiguousarray(img_u8.astype(np.uint8))
        qi = QtGui.QImage(img.data, img.shape[1], img.shape[0], img.shape[1],
                          QtGui.QImage.Format_Grayscale8)
        return QtGui.QPixmap.fromImage(qi.copy()).scaled(w, h, QtCore.Qt.KeepAspectRatio,
                                                         QtCore.Qt.SmoothTransformation)

    def _rgb_pix(self, bgr, w, h):
        rgb = np.ascontiguousarray(cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB))
        qi = QtGui.QImage(rgb.data, rgb.shape[1], rgb.shape[0], rgb.shape[1] * 3,
                          QtGui.QImage.Format_RGB888)
        return QtGui.QPixmap.fromImage(qi.copy()).scaled(w, h, QtCore.Qt.KeepAspectRatio,
                                                        QtCore.Qt.SmoothTransformation)

    def _show_live(self, frame):
        img = preprocess(frame, self.clear) if self.clear is not None else frame
        self.live.setPixmap(self._gray_pix(img, 80 * ZOOM, 64 * ZOOM))

    def _update_map(self):
        if not self.session or not self.session.frags:
            return
        vis, frac = self.session.render_main()
        if vis is not None:
            self.map_label.setPixmap(self._rgb_pix(vis, MAP_W, MAP_H))
        main_tiles = len(self.session.main().kfs)
        enough = "· copertura buona ✓" if (main_tiles >= 60 and frac < 0.10) else "· continua a coprire"
        self.map_title.setText(
            f"Mappa — {main_tiles} tessere · {len(self.session.frags)} pezzi · buchi {frac * 100:.0f}%  {enough}")
        # strip dei pezzi da incastrare (i piu' grandi)
        while self.pieces_l.count():
            w = self.pieces_l.takeAt(0).widget()
            if w:
                w.deleteLater()
        pieces = sorted(self.session.pieces(), key=lambda p: -p[1])[:12]
        for mos, n in pieces:
            lbl = QtWidgets.QLabel()
            lbl.setPixmap(self._gray_pix(mos, 80 * 2, 64 * 2))
            lbl.setToolTip(f"pezzo da {n} tessere")
            lbl.setStyleSheet("border:1px solid #556;border-radius:4px;")
            self.pieces_l.addWidget(lbl)

    # ---- auto-load ----
    def _autoload(self):
        files = glob.glob(os.path.join(STATE_DIR, "template_*.npz"))
        for path in sorted(files, key=os.path.getmtime, reverse=True):
            try:
                self.session = FM.FragmentMapper.load(path)
                self.clear = self.session.clear
                self._update_map()
                tot = sum(len(f.kfs) for f in self.session.frags)
                self.status.setText(f"Mappa caricata: {os.path.basename(path)} "
                                    f"({tot} tessere, {len(self.session.frags)} pezzi). Inizializza per continuare o verificare.")
                self.btn_verify.setEnabled(True); self.btn_save.setEnabled(True)
                return
            except Exception:
                continue   # formato incompatibile: salta

    # ---- azioni ----
    def on_init(self):
        self.btn_init.setEnabled(False)
        self.status.setText("Inizializzo il sensore (TLS-PSK)…")
        self.request_open.emit()

    @QtCore.Slot(object)
    def on_clear(self, clear):
        self.clear = clear
        if self.session is None:
            self.session = FM.FragmentMapper(clear)
        else:
            self.session.clear = clear
        self.ready = True
        self.status.setText("Pronto. «Mappa dito» e muovi lentamente il dito per cucire la mappa.")
        self.btn_map.setEnabled(True); self.btn_verify.setEnabled(True); self.btn_save.setEnabled(True)
        self._show_live(clear)

    def _capture(self, kind):
        if self.ready:
            self.request_capture.emit(kind)

    def on_toggle_map(self):
        if not self.ready:
            self.btn_map.setChecked(False); return
        self.mapping = self.btn_map.isChecked()
        if self.mapping:
            self.btn_map.setText("Ferma mappatura"); self.btn_verify.setEnabled(False)
            self._capture("map")
        else:
            self.btn_map.setText("Mappa dito"); self.btn_verify.setEnabled(True)

    @QtCore.Slot(object, str)
    def on_frame(self, frame, kind):
        self._show_live(frame)
        if kind == "map":
            r = self.session.add_frame(frame)
            if r.get("placed"):
                self._update_map()
            tag = {"mappa estesa": "✓", "inizio mappa": "✓", "nuovo pezzo": "+",
                   "già coperto qui": "•", "debole": "·"}.get(
                r["status"], "↔" if r["status"].startswith("ponte") else "·")
            extra = f"  (inlier {r['inl']})" if "inl" in r else ""
            self.status.setText(f"{tag} {r['status']}{extra} — {r['frags']} pezzi")
            if self.mapping:
                self._capture("map")
        else:
            ok, score, inl = self.session.verify(frame)
            self._vscores.append(int(score)); self._vframes.append(frame); self._vleft -= 1
            self.status.setText(f"Verifica… {len(self._vscores)}/{VERIFY_FRAMES}  (miglior score {max(self._vscores)})")
            if self._vleft > 0:
                self._capture("verify")
            else:
                self._finish_verify()

    def on_verify_start(self):
        if not self.session or not self.session.frags:
            self.status.setText("Nessuna mappa: mappa il dito o carica una mappa salvata."); return
        self._vscores, self._vframes, self._vleft = [], [], VERIFY_FRAMES
        self.btn_verify.setEnabled(False)
        self.status.setText("Verifica: appoggia il dito e TIENILO FERMO un secondo…")
        self._capture("verify")

    def _finish_verify(self):
        best = max(self._vscores) if self._vscores else 0
        npass = sum(s >= FM.VERIFY_MIN for s in self._vscores)
        ok = best >= FM.VERIFY_MIN
        self._log_verify(best, ok)
        self.btn_verify.setEnabled(True)
        self.status.setText(
            f"{'✓ ACCETTATO' if ok else '✗ RIFIUTATO'} — miglior score {best} / soglia {FM.VERIFY_MIN}"
            f"  ·  frame che passano: {npass}/{len(self._vscores)}  ·  score: {self._vscores}")

    def _log_verify(self, best, ok):
        try:
            os.makedirs(LOG_DIR, exist_ok=True)
            ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            np.savez_compressed(os.path.join(LOG_DIR, f"verify_{ts}.npz"),
                                frames=np.array(self._vframes, dtype=np.uint16),
                                clear=self.session.clear.astype(np.uint16),
                                scores=np.array(self._vscores, np.int32),
                                accepted=np.array([ok]))
        except Exception:
            pass

    def on_save(self):
        if not self.session or not self.session.frags:
            self.status.setText("Niente da salvare: mappa prima il dito."); return
        os.makedirs(STATE_DIR, exist_ok=True)
        removed = self.session.prune(2)        # scarta i pezzetti inutili (bordi/rumore)
        if removed:
            self._update_map()
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        path = os.path.join(STATE_DIR, f"template_{ts}.npz")
        self.session.save(path)
        tot = sum(len(f.kfs) for f in self.session.frags)
        self.status.setText(f"Mappa salvata: {os.path.basename(path)} ({tot} tessere, "
                            f"{len(self.session.frags)} pezzi, potati {removed} pezzetti)")

    @QtCore.Slot(str)
    def on_failed(self, msg):
        self.status.setText(f"⚠ {msg}")
        self.btn_init.setEnabled(True)
        if self.mapping:
            self.mapping = False
            self.btn_map.setChecked(False); self.btn_map.setText("Mappa dito")
            self.btn_verify.setEnabled(True)

    def closeEvent(self, ev):
        self.mapping = False
        self.worker.stop = True
        self.thread.quit(); self.thread.wait(4000)
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
    t = QtCore.QTimer(); t.start(200); t.timeout.connect(lambda: None)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
