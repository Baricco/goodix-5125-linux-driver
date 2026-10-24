#!/usr/bin/env python3
"""Esaltazione delle creste orientata (banco di filtri di Gabor) per immagini del 5125.

Rende le feature piu' stabili tra pressioni diverse: stima l'orientamento locale delle creste
e applica il filtro di Gabor allineato. Base per un matching robusto su sensore piccolo.
"""
import numpy as np
import cv2


def ridge_orientation(img, block=8):
    gx = cv2.Sobel(img, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(img, cv2.CV_32F, 0, 1, ksize=3)
    gxx = cv2.boxFilter(gx * gx, -1, (block, block))
    gyy = cv2.boxFilter(gy * gy, -1, (block, block))
    gxy = cv2.boxFilter(gx * gy, -1, (block, block))
    return 0.5 * np.arctan2(2 * gxy, gxx - gyy)  # orientamento del gradiente


def gabor_enhance(u8, wavelength=6.0):
    img = u8.astype(np.float32) / 255.0
    orient = ridge_orientation(img) + np.pi / 2  # ruota sulle creste
    out = np.zeros_like(img)
    # banco su angoli discreti, seleziona per pixel l'angolo locale
    angles = np.linspace(0, np.pi, 12, endpoint=False)
    responses = []
    for a in angles:
        k = cv2.getGaborKernel((9, 9), 2.5, a, wavelength, 0.6, 0, ktype=cv2.CV_32F)
        responses.append(cv2.filter2D(img, cv2.CV_32F, k))
    responses = np.stack(responses, 0)
    idx = np.clip((orient % np.pi) / np.pi * 12, 0, 11).astype(int)
    for a in range(12):
        m = idx == a
        out[m] = responses[a][m]
    out = cv2.normalize(out, None, 0, 255, cv2.NORM_MINMAX)
    return out.astype(np.uint8)
