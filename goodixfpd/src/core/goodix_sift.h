/* goodix_sift.h - SIFT feature extraction without OpenCV (host-side matching)
 *
 * Matches OpenCV cv::SIFT default parameters item by item (nOctaveLayers=3,
 * contrastThreshold=0.04, edgeThreshold=10, sigma=1.6, first octave upsampled
 * 2x); outputs keypoint coordinates (original image pixels) and 128-dimension
 * descriptors (each 0..255).
 * Plain C, no global state, reentrant.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GOODIX_SIFT_H
#define GOODIX_SIFT_H

#include <stdint.h>

#define GX_SIFT_DESC_LEN 128

/* One keypoint: position (original image coordinates) + descriptor */
struct gx_feat {
    float x, y;
    uint8_t desc[GX_SIFT_DESC_LEN];
};

/* The feature set of one frame */
struct gx_featset {
    uint32_t n;
    struct gx_feat *f;      /* malloc'ed, freed by gx_featset_clear */
};

/* Extracts SIFT features from an 8-bit grayscale image (w*h, row major).
 * Returns 0 on success (out->n may be 0); -1 on invalid arguments or
 * allocation failure. */
int gx_sift_extract(const uint8_t *img8, int w, int h, struct gx_featset *out);

void gx_featset_clear(struct gx_featset *fs);

#endif /* GOODIX_SIFT_H */
