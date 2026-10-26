/* goodix_imgproc.h - parameterized image preprocessing pipeline (input of
 * host-side matching)
 *
 * A 16-bit frame -> 8-bit grayscale pipeline independent of the
 * protocol/capture code, shared by the libfprint SIGFM matching and CLI
 * --capture (both entry points give the same output); every stage can be
 * tuned through environment variables (GOODIX_IMGPROC_*) or
 * struct gx_imgproc_params, for calibration on the real device.
 *
 * Pipeline (called per frame, no static buffers, reentrant):
 *   1. baseline    optional subtraction of the no-finger baseline (signed
 *                  difference + 2048 headroom, clamped to [0,4095])
 *   2. flatfield   optional removal of the low-frequency background
 *                  (separable box mean, window much larger than the ridge
 *                  spacing)
 *   3. stretch     1%/99% percentile clipping mapped to 0..255
 *   4. enhance     optional unsharp-mask local contrast boost (for SIGFM/SIFT's
 *                  contrastThreshold=0.04, so more weak-ridge keypoints pass)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GOODIX_IMGPROC_H
#define GOODIX_IMGPROC_H

#include <stdint.h>

struct goodix_dev;          /* defined in goodix.h, only forward-declared here */

/* enhancement mode */
enum gx_imgproc_enhance {
    GX_IMGPROC_ENHANCE_NONE = 0,     /* no enhancement (default) */
    GX_IMGPROC_ENHANCE_SIGFM,        /* unsharp mask: out = in + boost*(in - gauss(in,σ)) */
};

/* pipeline mode */
enum gx_imgproc_mode {
    GX_IMGPROC_MODE_LEGACY = 0,      /* baseline/flatfield/stretch/enhance (original pipeline) */
    GX_IMGPROC_MODE_RIDGE,           /* ridge band-pass: baseline -> [de-ghost] -> DoG band-pass
                                      * -> percentile stretch -> CLAHE (floating point, no 4095 clipping) */
};

/* Processing parameters. Defaults in GX_IMGPROC_DEFAULT_PARAMS. */
struct gx_imgproc_params {
    uint8_t  baseline;          /* 1: subtract the no-finger baseline (d->img_base); 0: use the raw frame */
    uint16_t baseline_offset;   /* signed difference headroom (default 2048, fits differences both ways) */
    uint8_t  flatfield;         /* 1: low-frequency flat-field correction */
    int      flatfield_r;       /* box mean radius (default 12, window 2r+1 ≈ 25 px) */
    uint8_t  pct_lo, pct_hi;    /* kept range [pct_lo%, pct_hi%] (default 1/99 = clip 1% at the
                                  bottom and at the top), mapped to 0..255 after clamping */
    uint8_t  enhance;           /* GX_IMGPROC_ENHANCE_* */
    float    enhance_boost;     /* unsharp-mask gain (default 0.8) */
    float    enhance_sigma;     /* unsharp-mask Gaussian σ (default 1.5, pixels) */
    /* ---- GX_IMGPROC_MODE_RIDGE ---- */
    uint8_t  mode;              /* GX_IMGPROC_MODE_* */
    uint8_t  fresh_base;        /* 1: when d->img_fresh exists, subtract it instead of d->img_base */
    uint8_t  deghost;           /* 1: remove by regression the gain residue shaped like the baseline (see .c) */
    float    dog_s1, dog_s2;    /* DoG band-pass σ (default 1.0 / 4.0 pixels, ridge spacing ~8-10 px) */
    float    clahe_clip;        /* CLAHE clip limit (default 2.5; <= 0 disables it) */
    uint8_t  clahe_tiles;       /* CLAHE tiles per axis (default 4 -> 20x16 tiles on 80x64) */
};

#define GX_IMGPROC_DEFAULT_PARAMS \
    { 1, 2048, 1, 12, 1, 99, GX_IMGPROC_ENHANCE_NONE, 0.8f, 1.5f, \
      GX_IMGPROC_MODE_LEGACY, 0, 0, 1.0f, 4.0f, 2.5f, 4 }
/* Default for the SIGFM driver path: RIDGE pipeline + fresh baseline +
 * de-ghosting. The old legacy path (flat field + stretch + unsharp mask)
 * stretched the pixel-grid residue over the whole image in weak presses;
 * SIFT keypoints landed on the residue and different fingers matched each
 * other with high scores (measured impostor scores 2000-13000).
 * The legacy fields stay for the GOODIX_IMGPROC_MODE=legacy fallback.
 * Environment variables (GOODIX_IMGPROC_*) can override them; after changing
 * parameters you must enroll again. */
#define GX_IMGPROC_SIGFM_PARAMS \
    { 1, 2048, 1, 12, 1, 99, GX_IMGPROC_ENHANCE_SIGFM, 0.8f, 1.5f, \
      GX_IMGPROC_MODE_RIDGE, 1, 1, 1.0f, 4.0f, 2.5f, 4 }

/* Frame signal statistics (for the host-side quality gate / calibration
 * logs), all in raw 12-bit counts:
 *   dc    = mean(raw - base): how much the finger lowers the readings overall
 *           (a full press measures ~ -1200)
 *   ridge = standard deviation of (raw - base) after the DoG band-pass: the
 *           ridge/valley contrast (empty frame / weak touch ≈ noise) */
struct gx_imgproc_signal {
    float dc;
    float ridge;
};

/* Computes the signal statistics of img16 against the baseline (img_fresh or
 * img_base depending on p->fresh_base). Returns -1 if no baseline is
 * available. */
int gx_imgproc_signal(const struct goodix_dev *d,
                      const struct gx_imgproc_params *p,
                      const uint8_t *img16, struct gx_imgproc_signal *out);

/* Calibration dump: with GOODIX_DUMP_IMGPROC=1 the 16-bit frame is written
 * as is to imgproc-<seq>-<stage>.pgm (P5, maxval 4095, big endian), seq
 * being the number of the next frame. Used to replay the whole pipeline
 * offline (raw16/base16/pre16). */
void gx_imgproc_dump16(const struct goodix_dev *d, const char *stage,
                       const uint8_t *img16);

/* Main entry point: processes according to the parameters. img16 is a
 * w*h*2-byte 16-bit LE frame, out8 is w*h bytes.
 * Returns 0 on success; -1 on invalid parameters (w/h out of range,
 * radius < 0, invalid percentiles).
 * With GOODIX_DUMP_IMGPROC=1 every stage is written to
 * gx_state_dir()/imgproc-<seq>-<stage>.pgm (raw8/base8/flat8/final8/enh8),
 * for calibration on the real device. */
int gx_imgproc_to8bit(const struct goodix_dev *d,
                      const struct gx_imgproc_params *p,
                      const uint8_t *img16, uint8_t *out8);

/* Overrides parameters from environment variables (each field separately;
 * unset variables keep their value):
 *   GOODIX_IMGPROC_BASELINE=0|1
 *   GOODIX_IMGPROC_FLATFIELD=0|1
 *   GOODIX_IMGPROC_FLATFIELD_R=<int>
 *   GOODIX_IMGPROC_PCT_LO=<int>  GOODIX_IMGPROC_PCT_HI=<int>
 *   GOODIX_IMGPROC_ENHANCE=none|sigfm
 *   GOODIX_IMGPROC_BOOST=<float>  GOODIX_IMGPROC_SIGMA=<float>
 *   GOODIX_IMGPROC_MODE=legacy|ridge
 *   GOODIX_IMGPROC_FRESH_BASE=0|1  GOODIX_IMGPROC_DEGHOST=0|1
 *   GOODIX_IMGPROC_DOG_S1=<float>  GOODIX_IMGPROC_DOG_S2=<float>
 *   GOODIX_IMGPROC_CLAHE=<float>   GOODIX_IMGPROC_CLAHE_TILES=<int>
 * Invalid values are silently ignored. */
void gx_imgproc_apply_env(struct gx_imgproc_params *p);

#endif /* GOODIX_IMGPROC_H */
