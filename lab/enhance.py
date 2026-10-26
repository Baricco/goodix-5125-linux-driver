#!/usr/bin/env python3
"""Orientation-aware ridge enhancement (bank of Gabor filters) for 5125 images.

Makes features more stable across different presses: it estimates the local ridge orientation
and applies the matching Gabor filter. A basis for robust matching on a small sensor.
"""
import numpy as np
import cv2


def ridge_orientation(img, block=8):
    gx = cv2.Sobel(img, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(img, cv2.CV_32F, 0, 1, ksize=3)
    gxx = cv2.boxFilter(gx * gx, -1, (block, block))
    gyy = cv2.boxFilter(gy * gy, -1, (block, block))
    gxy = cv2.boxFilter(gx * gy, -1, (block, block))
    return 0.5 * np.arctan2(2 * gxy, gxx - gyy)  # gradient orientation


def gabor_enhance(u8, wavelength=6.0):
    img = u8.astype(np.float32) / 255.0
    orient = ridge_orientation(img) + np.pi / 2  # rotate onto the ridges
    out = np.zeros_like(img)
    # bank over discrete angles, pick the local angle per pixel
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
