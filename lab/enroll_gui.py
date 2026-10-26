#!/usr/bin/env python3
"""Fingerprint management GUI for the Goodix 27c6:5125 (PySide6).

Manages a COLLECTION of named fingerprints: enroll as many as you like, map them (puzzle mosaic,
see fragment_mapper), and verification compares a touch against ALL of them, showing which one
matches. You can rename/delete to manage the collection.

Fingerprints live in state/fingers/<name>.npz. Runs as the user (udev uaccess). Matching is SIGFM
(0 false positives in the tests); verification is multi-frame (a genuine touch passes if at least
one frame locks).

Usage:
  tools/.venv/bin/python lab/enroll_gui.py
  tools/.venv/bin/python lab/enroll_gui.py --replay captures      # demo without hardware
"""

import os
import sys
import re
import time
import glob
import signal
import contextlib

# silence Qt's noise on Wayland (harmless but intrusive)
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


# --------------------------------------------------------------------- fingerprint store
def _safe(name):
    return re.sub(r"[^\w\-]+", "_", name.strip())[:40] or "fingerprint"


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


# --------------------------------------------------------------------- frame sources
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
            self.failed.emit(f"opening the sensor: {e}")

    @QtCore.Slot(str)
    def do_capture(self, kind):
        if self.stop:
            return
        try:
            with contextlib.redirect_stdout(DEVNULL):
                if kind == "map":
                    f = self.src.grab()          # mapping: continuous capture, no FDT dance
                else:
                    self.src.arm()               # verification: a single deliberate touch
                    f = self.src.capture_touch()
                    self.src.lift()
            self.frame_ready.emit(f, kind)
        except Exception as e:
            self.failed.emit(f"cattura: {e}")


# --------------------------------------------------------------------- window
class MainWindow(QtWidgets.QMainWindow):
    request_open = QtCore.Signal()
    request_capture = QtCore.Signal(str)

    def __init__(self, src):
        super().__init__()
        self.setWindowTitle("Goodix 5125 — Fingerprints")
        self.setStyleSheet(STYLE)
        self.resize(960, 600)
        self.store = FingerStore()
        self.fingers = {}          # name -> FragmentMapper (loaded, for verification)
        self.session = None        # active fingerprint (mapping/view)
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
        # left column: collection + sensor
        self.list = QtWidgets.QListWidget()
        self.list.itemSelectionChanged.connect(self.on_select)
        self.btn_new = QtWidgets.QPushButton("＋ New fingerprint", objectName="primary")
        self.btn_del = QtWidgets.QPushButton("🗑 Delete")
        self.btn_new.clicked.connect(self.on_new)
        self.btn_del.clicked.connect(self.on_delete)
        self.btn_init = QtWidgets.QPushButton("Initialize sensor")
        self.btn_init.clicked.connect(self.on_init)
        self.sensor_lbl = QtWidgets.QLabel("Sensor not initialized", objectName="status")

        left = QtWidgets.QVBoxLayout()
        left.addWidget(QtWidgets.QLabel("Fingerprints", objectName="title"))
        left.addWidget(self.list, 1)
        row = QtWidgets.QHBoxLayout(); row.addWidget(self.btn_new); row.addWidget(self.btn_del)
        left.addLayout(row)
        left.addSpacing(10)
        left.addWidget(self.btn_init)
        left.addWidget(self.sensor_lbl)
        left_w = QtWidgets.QWidget(); left_w.setLayout(left); left_w.setFixedWidth(240)

        # centre: map + actions
        self.map_title = QtWidgets.QLabel("No fingerprint selected", objectName="title")
        # current view (live frame in real time)
        self.live = QtWidgets.QLabel(alignment=QtCore.Qt.AlignCenter)
        self.live.setFixedSize(80 * 2, 64 * 2)
        self.live.setStyleSheet("background:#0c0e13;border:1px solid #2a2f3a;border-radius:6px;")
        self.map_label = QtWidgets.QLabel(objectName="map", alignment=QtCore.Qt.AlignCenter)
        self.map_label.setMinimumSize(MAP_W, MAP_H)
        self.btn_map = QtWidgets.QPushButton("Map finger", objectName="danger", checkable=True)
        self.btn_save = QtWidgets.QPushButton("Save")
        self.btn_verify = QtWidgets.QPushButton("Verify (all)", objectName="primary")
        self.btn_map.clicked.connect(self.on_toggle_map)
        self.btn_save.clicked.connect(self.on_save)
        self.btn_verify.clicked.connect(self.on_verify_start)
        self.status = QtWidgets.QLabel("Initialize the sensor, then create or select a fingerprint.",
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
        """Shows the current view (every frame, in real time)."""
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
            self.map_title.setText(f"{name}: empty — press «Map finger»" if self.session
                                   else "No fingerprint selected")
            return
        vis, frac = self.session.render_main()
        if vis is not None:
            self.map_label.setPixmap(self._rgb_pix(vis))
        fr = self.session.view()
        tiles = len(fr.kfs); is_main = fr is self.session.main()
        total = sum(len(f.kfs) for f in self.session.frags)
        piece = "main piece" if is_main else "⚠ detached piece"
        enough = " · good coverage ✓" if (is_main and tiles >= 60 and frac < 0.10) else ""
        self.map_title.setText(f"{self.active_name or 'unnamed'} — {piece}: {tiles} tiles · "
                               f"{len(self.session.frags)} pieces · {total} total · holes {frac * 100:.0f}%{enough}")

    # ---------------- collection ----------------
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
            self.list.addItem(f"{nm}   ·   {tiles} tiles")
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
            self.session.clear = self.clear      # to keep mapping with the fresh clear frame
        self._show_map()
        self.status.setText(f"Fingerprint «{nm}» selected. You can keep mapping it or verify.")
        self._sync_buttons()

    def on_new(self):
        if not self.ready:
            self.status.setText("Initialize the sensor first."); return
        name, ok = QtWidgets.QInputDialog.getText(self, "New fingerprint", "Name (e.g. right index):")
        if not ok or not name.strip():
            return
        if self.store.exists(name):
            self.status.setText(f"A fingerprint «{name}» already exists. Choose another name."); return
        self.active_name = name.strip()
        self.session = FM.FragmentMapper(self.clear)
        self.list.clearSelection()
        self._show_map()
        self.status.setText(f"«{self.active_name}»: press «Map finger» and move the finger. Then «Save».")
        self._sync_buttons()

    def on_delete(self):
        nm = self._name_of(self.list.currentItem())
        if not nm:
            return
        if QtWidgets.QMessageBox.question(self, "Delete", f"Delete the fingerprint «{nm}»?") \
                != QtWidgets.QMessageBox.Yes:
            return
        self.store.delete(nm)
        if self.active_name == nm:
            self.session = None; self.active_name = None; self._show_map()
        self._reload_fingers()
        self.status.setText(f"Fingerprint «{nm}» deleted.")

    def on_save(self):
        if not self.session or not self._tiles():
            self.status.setText("Nothing to save: map the finger first."); return
        if not self.active_name:
            name, ok = QtWidgets.QInputDialog.getText(self, "Save fingerprint", "Name:")
            if not ok or not name.strip():
                return
            self.active_name = name.strip()
        removed = self.session.prune(2)
        self.store.save(self.active_name, self.session)
        self._reload_fingers()
        # select it again
        for i in range(self.list.count()):
            if self._name_of(self.list.item(i)) == self.active_name:
                self.list.setCurrentRow(i); break
        self.status.setText(f"Saved «{self.active_name}» ({self._tiles()} tiles, "
                            f"pruned {removed} scraps).")

    # ---------------- sensor / mapping ----------------
    def on_init(self):
        self.btn_init.setEnabled(False)
        self.sensor_lbl.setText("Initializing (TLS-PSK)…")
        self.request_open.emit()

    @QtCore.Slot(object)
    def on_clear(self, clear):
        self.clear = clear
        self.ready = True
        if self.session is not None:
            self.session.clear = clear
        self.sensor_lbl.setText("Sensor ready ✓")
        self.status.setText("Ready. Create a «New fingerprint» or select one and verify.")
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
            self.btn_map.setText("Map finger"); self._show_map(); self._sync_buttons()

    def on_verify_start(self):
        if not self.fingers:
            self.status.setText("No saved fingerprint to verify."); return
        self._vleft = VERIFY_FRAMES
        self._vbest = {nm: 0 for nm in self.fingers}
        self.btn_verify.setEnabled(False); self.btn_map.setEnabled(False)
        self.status.setText("Verify: place your finger and HOLD IT STILL for a second…")
        self._capture("verify")

    @QtCore.Slot(object, str)
    def on_frame(self, frame, kind):
        self._show_live(frame)          # current view in real time, every frame
        if kind == "map":
            r = self.session.add_frame(frame)
            now = time.monotonic()
            # redraw the map (throttled ~8/s) on every LOCKED frame, so the "you are here" marker
            # follows the view even over areas already mapped; empty frames move nothing.
            if not r["status"].startswith("empty") and now - self._last_render > 0.12:
                self._show_map(); self._last_render = now
            tag = {"map extended": "✓", "map started": "✓", "new piece": "+",
                   "already covered here": "•", "weak": "·"}.get(
                r["status"], "↔" if r["status"].startswith("bridge") else "·")
            self.status.setText(f"{tag} {r['status']} — {r['frags']} pieces")
            if self.mapping:
                self._capture("map")
        else:  # verify against all of them
            import sigfm
            feats = sigfm.features(preprocess(frame, self.clear))
            for nm, fm in self.fingers.items():
                self._vbest[nm] = max(self._vbest[nm], fm.best_score(feats))
            self._vleft -= 1
            self.status.setText(f"Verifying… {VERIFY_FRAMES - self._vleft}/{VERIFY_FRAMES}")
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
            self.status.setText(f"✓ RECOGNIZED: «{winner}»  (score {score})   —   {detail}")
        else:
            self.status.setText(f"✗ no match (best {winner}:{score} < {FM.VERIFY_MIN})   —   {detail}")
        self._sync_buttons()

    @QtCore.Slot(str)
    def on_failed(self, msg):
        self.sensor_lbl.setText(f"⚠ {msg}")
        self.btn_init.setEnabled(True)
        if self.mapping:
            self.mapping = False; self.btn_map.setChecked(False); self.btn_map.setText("Map finger")
        self._sync_buttons()

    def closeEvent(self, ev):
        self.mapping = False
        self.worker.stop = True
        self.thread.quit()
        if not self.thread.wait(3000):
            # worker stuck in a libusb call (sensor hung): forced but clean exit
            # instead of letting Qt destroy the QThread -> SIGABRT.
            os._exit(0)
        # thread finished: no concurrent access -> clean teardown of the sensor
        try:
            if self.ready:
                self.worker.src.close()
        except Exception:
            pass
        super().closeEvent(ev)


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--replay", metavar="DIR", help="reads the *_plain.bin files from DIR instead of the hardware")
    args = ap.parse_args()
    src = ReplaySource(args.replay) if args.replay else HardwareSource()
    app = QtWidgets.QApplication(sys.argv)
    w = MainWindow(src); w.show()
    signal.signal(signal.SIGINT, lambda *a: w.close())
    t = QtCore.QTimer(); t.start(250); t.timeout.connect(lambda: None)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
