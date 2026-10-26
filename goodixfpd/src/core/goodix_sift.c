/* goodix_sift.c - SIFT (Lowe 2004) without OpenCV, aligned step by step with
 * cv::SIFT
 *
 * Same flow as OpenCV (features/sift.simd.hpp + sift.dispatch.cpp):
 *   1. convert the image to float, bilinear 2x upscale, Gaussian blur to
 *      σ=1.6 (first octave -1);
 *   2. Gaussian pyramid (nOctaveLayers+3 layers per octave) and DoG pyramid;
 *   3. 26-neighbour extrema -> 3D quadratic interpolation refinement ->
 *      contrast/edge response filtering;
 *   4. 36-bin orientation histogram, every peak above 80% of the main peak
 *      becomes a keypoint;
 *   5. 4x4x8 descriptor (trilinear interpolation, 0.2 clipping, x512
 *      quantization to 0..255).
 * Parameters are OpenCV's defaults; Gaussian kernel length round(8σ+1)|1,
 * reflect-101 border, consistent with GaussianBlur on CV_32F.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "goodix_sift.h"

#define SIFT_LAYERS          3
#define SIFT_CONTRAST        0.04f
#define SIFT_EDGE            10.0f
#define SIFT_SIGMA           1.6f
#define SIFT_INIT_SIGMA      0.5f
#define SIFT_IMG_BORDER      5
#define SIFT_MAX_INTERP      5
#define SIFT_ORI_BINS        36
#define SIFT_ORI_SIG_FCTR    1.5f
#define SIFT_ORI_RADIUS      (3 * SIFT_ORI_SIG_FCTR)
#define SIFT_ORI_PEAK        0.8f
#define SIFT_D               4          /* descriptor spatial grid 4x4 */
#define SIFT_N               8          /* orientation bins per cell */
#define SIFT_DESCR_SCL       3.0f
#define SIFT_DESCR_MAG_THR   0.2f
#define SIFT_INT_DESCR_FCTR  512.0f
#define SIFT_PI              3.14159265358979323846

struct fimg {
    int w, h;
    float *p;
};

#define PX(im, x, y) ((im)->p[(y) * (im)->w + (x)])

/* candidate keypoint (pyramid-internal coordinates) */
struct kp {
    int o, layer;           /* pyramid octave (0 = the 2x-upscaled octave) and layer */
    int ir, ic;             /* refined integer position (centre of the orientation histogram) */
    float x, y;             /* coordinates within the octave (with sub-pixel offset) */
    float scl;              /* scale σ within the octave */
    float angle;            /* OpenCV convention: 360 - main orientation */
    float ox, oy;           /* original image coordinates */
    float size;             /* original image scale (for the dedup sort) */
};

static int fimg_alloc(struct fimg *im, int w, int h)
{
    im->w = w;
    im->h = h;
    im->p = calloc((size_t)w * h, sizeof(float));
    return im->p ? 0 : -1;
}

static inline int cv_round(double v)
{
    return (int)lrint(v);           /* as cvRound: round to nearest, ties to even */
}

static inline int reflect101(int i, int n)
{
    if (n == 1)
        return 0;
    while (i < 0 || i >= n)
        i = i < 0 ? -i : 2 * n - 2 - i;
    return i;
}

/* separable Gaussian, kernel length cvRound(8σ+1)|1, reflect-101 border */
static int gauss(const struct fimg *src, struct fimg *dst, float sigma)
{
    int ks = cv_round(sigma * 8.0f + 1.0f) | 1;
    int r = ks / 2;
    int w = src->w, h = src->h;
    float *k = malloc((size_t)ks * sizeof(float));
    float *tmp = malloc((size_t)w * h * sizeof(float));
    float sum = 0.0f;

    if (!k || !tmp) {
        free(k);
        free(tmp);
        return -1;
    }
    for (int i = 0; i < ks; i++) {
        float x = (float)(i - r);
        k[i] = expf(-0.5f * x * x / (sigma * sigma));
        sum += k[i];
    }
    for (int i = 0; i < ks; i++)
        k[i] /= sum;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc = 0.0f;
            for (int j = -r; j <= r; j++)
                acc += k[j + r] * src->p[y * w + reflect101(x + j, w)];
            tmp[y * w + x] = acc;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc = 0.0f;
            for (int j = -r; j <= r; j++)
                acc += k[j + r] * tmp[reflect101(y + j, h) * w + x];
            dst->p[y * w + x] = acc;
        }
    free(k);
    free(tmp);
    return 0;
}

/* bilinear 2x upscale (pixel-centre aligned, replicated border, as resize
 * INTER_LINEAR) */
static void upscale2x(const uint8_t *src, int w, int h, struct fimg *dst)
{
    for (int y = 0; y < 2 * h; y++) {
        float fy = y * 0.5f - 0.25f;
        int y0 = (int)floorf(fy);
        float ay = fy - y0;
        int y1 = y0 + 1;
        y0 = y0 < 0 ? 0 : y0 > h - 1 ? h - 1 : y0;
        y1 = y1 < 0 ? 0 : y1 > h - 1 ? h - 1 : y1;
        for (int x = 0; x < 2 * w; x++) {
            float fx = x * 0.5f - 0.25f;
            int x0 = (int)floorf(fx);
            float ax = fx - x0;
            int x1 = x0 + 1;
            x0 = x0 < 0 ? 0 : x0 > w - 1 ? w - 1 : x0;
            x1 = x1 < 0 ? 0 : x1 > w - 1 ? w - 1 : x1;
            float top = src[y0 * w + x0] * (1 - ax) + src[y0 * w + x1] * ax;
            float bot = src[y1 * w + x0] * (1 - ax) + src[y1 * w + x1] * ax;
            dst->p[y * dst->w + x] = top * (1 - ay) + bot * ay;
        }
    }
}

/* nearest-neighbour downscale by half (as resize INTER_NEAREST,
 * dsize = src/2) */
static void downsample(const struct fimg *src, struct fimg *dst)
{
    double fx = (double)src->w / dst->w, fy = (double)src->h / dst->h;
    for (int y = 0; y < dst->h; y++) {
        int sy = (int)floor(y * fy);
        sy = sy > src->h - 1 ? src->h - 1 : sy;
        for (int x = 0; x < dst->w; x++) {
            int sx = (int)floor(x * fx);
            sx = sx > src->w - 1 ? src->w - 1 : sx;
            dst->p[y * dst->w + x] = src->p[sy * src->w + sx];
        }
    }
}

/* 3x3 linear system (Gaussian elimination with partial pivoting); x=0 when
 * singular, as when Matx::solve LU fails */
static void solve3(const float A_in[9], const float b_in[3], float x[3])
{
    double A[3][4];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            A[i][j] = A_in[i * 3 + j];
        A[i][3] = b_in[i];
    }
    for (int c = 0; c < 3; c++) {
        int p = c;
        for (int r = c + 1; r < 3; r++)
            if (fabs(A[r][c]) > fabs(A[p][c]))
                p = r;
        if (fabs(A[p][c]) < DBL_EPSILON) {
            x[0] = x[1] = x[2] = 0.0f;
            return;
        }
        if (p != c)
            for (int j = 0; j < 4; j++) {
                double t = A[c][j];
                A[c][j] = A[p][j];
                A[p][j] = t;
            }
        for (int r = c + 1; r < 3; r++) {
            double f = A[r][c] / A[c][c];
            for (int j = c; j < 4; j++)
                A[r][j] -= f * A[c][j];
        }
    }
    for (int i = 2; i >= 0; i--) {
        double s = A[i][3];
        for (int j = i + 1; j < 3; j++)
            s -= A[i][j] * x[j];
        x[i] = (float)(s / A[i][i]);
    }
}

/* 3D interpolation refinement of an extremum + contrast/edge filtering
 * (adjustLocalExtrema). dog is this octave's array of DoG layers. Returns 1
 * on success and fills kp. */
static int adjust_extremum(struct fimg *dog, int o, int layer, int r, int c,
                           struct kp *out)
{
    const float img_scale = 1.0f / 255.0f;
    const float deriv_scale = img_scale * 0.5f;
    const float second_scale = img_scale;
    const float cross_scale = img_scale * 0.25f;
    float xi = 0, xr = 0, xc = 0, contr;
    int i;

    for (i = 0; i < SIFT_MAX_INTERP; i++) {
        const struct fimg *img = &dog[layer], *prv = &dog[layer - 1], *nxt = &dog[layer + 1];
        float dD[3] = {
            (PX(img, c + 1, r) - PX(img, c - 1, r)) * deriv_scale,
            (PX(img, c, r + 1) - PX(img, c, r - 1)) * deriv_scale,
            (PX(nxt, c, r) - PX(prv, c, r)) * deriv_scale,
        };
        float v2 = PX(img, c, r) * 2;
        float dxx = (PX(img, c + 1, r) + PX(img, c - 1, r) - v2) * second_scale;
        float dyy = (PX(img, c, r + 1) + PX(img, c, r - 1) - v2) * second_scale;
        float dss = (PX(nxt, c, r) + PX(prv, c, r) - v2) * second_scale;
        float dxy = (PX(img, c + 1, r + 1) - PX(img, c - 1, r + 1) -
                     PX(img, c + 1, r - 1) + PX(img, c - 1, r - 1)) * cross_scale;
        float dxs = (PX(nxt, c + 1, r) - PX(nxt, c - 1, r) -
                     PX(prv, c + 1, r) + PX(prv, c - 1, r)) * cross_scale;
        float dys = (PX(nxt, c, r + 1) - PX(nxt, c, r - 1) -
                     PX(prv, c, r + 1) + PX(prv, c, r - 1)) * cross_scale;
        float H[9] = { dxx, dxy, dxs, dxy, dyy, dys, dxs, dys, dss };
        float X[3];

        solve3(H, dD, X);
        xi = -X[2];
        xr = -X[1];
        xc = -X[0];
        if (fabsf(xi) < 0.5f && fabsf(xr) < 0.5f && fabsf(xc) < 0.5f)
            break;
        if (fabsf(xi) > (float)(1 << 28) || fabsf(xr) > (float)(1 << 28) ||
            fabsf(xc) > (float)(1 << 28))
            return 0;
        c += cv_round(xc);
        r += cv_round(xr);
        layer += cv_round(xi);
        if (layer < 1 || layer > SIFT_LAYERS ||
            c < SIFT_IMG_BORDER || c >= dog[0].w - SIFT_IMG_BORDER ||
            r < SIFT_IMG_BORDER || r >= dog[0].h - SIFT_IMG_BORDER)
            return 0;
    }
    if (i >= SIFT_MAX_INTERP)
        return 0;

    {
        const struct fimg *img = &dog[layer], *prv = &dog[layer - 1], *nxt = &dog[layer + 1];
        float dD[3] = {
            (PX(img, c + 1, r) - PX(img, c - 1, r)) * deriv_scale,
            (PX(img, c, r + 1) - PX(img, c, r - 1)) * deriv_scale,
            (PX(nxt, c, r) - PX(prv, c, r)) * deriv_scale,
        };
        float t = dD[0] * xc + dD[1] * xr + dD[2] * xi;
        float v2, dxx, dyy, dxy, tr, det;

        contr = PX(img, c, r) * img_scale + t * 0.5f;
        if (fabsf(contr) * SIFT_LAYERS < SIFT_CONTRAST)
            return 0;
        v2 = PX(img, c, r) * 2.0f;
        dxx = (PX(img, c + 1, r) + PX(img, c - 1, r) - v2) * second_scale;
        dyy = (PX(img, c, r + 1) + PX(img, c, r - 1) - v2) * second_scale;
        dxy = (PX(img, c + 1, r + 1) - PX(img, c - 1, r + 1) -
               PX(img, c + 1, r - 1) + PX(img, c - 1, r - 1)) * cross_scale;
        tr = dxx + dyy;
        det = dxx * dyy - dxy * dxy;
        if (det <= 0 || tr * tr * SIFT_EDGE >= (SIFT_EDGE + 1) * (SIFT_EDGE + 1) * det)
            return 0;
    }

    out->o = o;
    out->layer = layer;
    out->ir = r;
    out->ic = c;
    out->x = c + xc;
    out->y = r + xr;
    out->scl = SIFT_SIGMA * powf(2.0f, (layer + xi) / SIFT_LAYERS);
    /* upscaled octave -1: original coordinate = octave coordinate * 2^o / 2 */
    out->ox = out->x * (float)(1 << o) * 0.5f;
    out->oy = out->y * (float)(1 << o) * 0.5f;
    out->size = out->scl * (float)(1 << o) * 2.0f * 0.5f;
    return 1;
}

/* orientation histogram (calcOrientationHist), returns the maximum */
static float orientation_hist(const struct fimg *img, int px, int py,
                              int radius, float sigma, float hist[SIFT_ORI_BINS])
{
    const int n = SIFT_ORI_BINS;
    float temp[SIFT_ORI_BINS + 4] = { 0 };
    float *th = temp + 2;
    float expf_scale = -1.0f / (2.0f * sigma * sigma);
    float maxval;

    for (int i = -radius; i <= radius; i++) {
        int y = py + i;
        if (y <= 0 || y >= img->h - 1)
            continue;
        for (int j = -radius; j <= radius; j++) {
            int x = px + j;
            float dx, dy, ori, mag, wgt;
            int bin;
            if (x <= 0 || x >= img->w - 1)
                continue;
            dx = PX(img, x + 1, y) - PX(img, x - 1, y);
            dy = PX(img, x, y - 1) - PX(img, x, y + 1);
            wgt = expf((i * i + j * j) * expf_scale);
            ori = atan2f(dy, dx) * (float)(180.0 / SIFT_PI);
            if (ori < 0)
                ori += 360.0f;
            mag = sqrtf(dx * dx + dy * dy);
            bin = cv_round((n / 360.0f) * ori);
            if (bin >= n)
                bin -= n;
            if (bin < 0)
                bin += n;
            th[bin] += wgt * mag;
        }
    }
    th[-1] = th[n - 1];
    th[-2] = th[n - 2];
    th[n] = th[0];
    th[n + 1] = th[1];
    for (int i = 0; i < n; i++)
        hist[i] = (th[i - 2] + th[i + 2]) * (1.0f / 16.0f) +
                  (th[i - 1] + th[i + 1]) * (4.0f / 16.0f) +
                  th[i] * (6.0f / 16.0f);
    maxval = hist[0];
    for (int i = 1; i < n; i++)
        if (hist[i] > maxval)
            maxval = hist[i];
    return maxval;
}

/* descriptor (calcSIFTDescriptor): ori is the main orientation (degrees),
 * scl the scale within the octave */
static void descriptor(const struct fimg *img, float fx, float fy, float ori,
                       float scl, uint8_t out[GX_SIFT_DESC_LEN])
{
    const int d = SIFT_D, n = SIFT_N;
    float hist[(SIFT_D + 2) * (SIFT_D + 2) * (SIFT_N + 2)] = { 0 };
    float dst[GX_SIFT_DESC_LEN];
    int px = cv_round(fx), py = cv_round(fy);
    float cos_t = cosf(ori * (float)(SIFT_PI / 180));
    float sin_t = sinf(ori * (float)(SIFT_PI / 180));
    float bins_per_rad = n / 360.0f;
    float exp_scale = -1.0f / (d * d * 0.5f);
    float hist_width = SIFT_DESCR_SCL * scl;
    int radius = cv_round(hist_width * 1.4142135623730951f * (d + 1) * 0.5f);
    int diag = (int)sqrt((double)img->w * img->w + (double)img->h * img->h);
    float nrm2 = 0, thr;

    if (radius > diag)
        radius = diag;
    cos_t /= hist_width;
    sin_t /= hist_width;

    for (int i = -radius; i <= radius; i++)
        for (int j = -radius; j <= radius; j++) {
            float c_rot = j * cos_t - i * sin_t;
            float r_rot = j * sin_t + i * cos_t;
            float rbin = r_rot + (float)(d / 2) - 0.5f;   /* integer division d/2, as in OpenCV */
            float cbin = c_rot + (float)(d / 2) - 0.5f;
            int r = py + i, c = px + j;
            float dx, dy, gori, mag, obin;
            int r0, c0, o0, idx;

            if (!(rbin > -1 && rbin < d && cbin > -1 && cbin < d &&
                  r > 0 && r < img->h - 1 && c > 0 && c < img->w - 1))
                continue;
            dx = PX(img, c + 1, r) - PX(img, c - 1, r);
            dy = PX(img, c, r - 1) - PX(img, c, r + 1);
            gori = atan2f(dy, dx) * (float)(180.0 / SIFT_PI);
            if (gori < 0)
                gori += 360.0f;
            mag = sqrtf(dx * dx + dy * dy) *
                  expf((c_rot * c_rot + r_rot * r_rot) * exp_scale);
            obin = (gori - ori) * bins_per_rad;

            r0 = (int)floorf(rbin);
            c0 = (int)floorf(cbin);
            o0 = (int)floorf(obin);
            rbin -= r0;
            cbin -= c0;
            obin -= o0;
            if (o0 < 0)
                o0 += n;
            if (o0 >= n)
                o0 -= n;

            {
                float v_r1 = mag * rbin, v_r0 = mag - v_r1;
                float v_rc11 = v_r1 * cbin, v_rc10 = v_r1 - v_rc11;
                float v_rc01 = v_r0 * cbin, v_rc00 = v_r0 - v_rc01;
                float v_rco111 = v_rc11 * obin, v_rco110 = v_rc11 - v_rco111;
                float v_rco101 = v_rc10 * obin, v_rco100 = v_rc10 - v_rco101;
                float v_rco011 = v_rc01 * obin, v_rco010 = v_rc01 - v_rco011;
                float v_rco001 = v_rc00 * obin, v_rco000 = v_rc00 - v_rco001;

                idx = ((r0 + 1) * (d + 2) + c0 + 1) * (n + 2) + o0;
                hist[idx] += v_rco000;
                hist[idx + 1] += v_rco001;
                hist[idx + (n + 2)] += v_rco010;
                hist[idx + (n + 3)] += v_rco011;
                hist[idx + (d + 2) * (n + 2)] += v_rco100;
                hist[idx + (d + 2) * (n + 2) + 1] += v_rco101;
                hist[idx + (d + 3) * (n + 2)] += v_rco110;
                hist[idx + (d + 3) * (n + 2) + 1] += v_rco111;
            }
        }

    for (int i = 0; i < d; i++)
        for (int j = 0; j < d; j++) {
            int idx = ((i + 1) * (d + 2) + (j + 1)) * (n + 2);
            hist[idx] += hist[idx + n];
            hist[idx + 1] += hist[idx + n + 1];
            for (int k = 0; k < n; k++)
                dst[(i * d + j) * n + k] = hist[idx + k];
        }
    for (int k = 0; k < GX_SIFT_DESC_LEN; k++)
        nrm2 += dst[k] * dst[k];
    thr = sqrtf(nrm2) * SIFT_DESCR_MAG_THR;
    nrm2 = 0;
    for (int k = 0; k < GX_SIFT_DESC_LEN; k++) {
        if (dst[k] > thr)
            dst[k] = thr;
        nrm2 += dst[k] * dst[k];
    }
    nrm2 = SIFT_INT_DESCR_FCTR / fmaxf(sqrtf(nrm2), FLT_EPSILON);
    for (int k = 0; k < GX_SIFT_DESC_LEN; k++) {
        int v = cv_round(dst[k] * nrm2);
        out[k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

/* sort key of KeyPointsFilter::removeDuplicatedSorted: x, y ascending, size
 * descending, angle ascending */
static int kp_cmp(const void *a, const void *b)
{
    const struct kp *p = a, *q = b;
    if (p->ox != q->ox)
        return p->ox < q->ox ? -1 : 1;
    if (p->oy != q->oy)
        return p->oy < q->oy ? -1 : 1;
    if (p->size != q->size)
        return p->size > q->size ? -1 : 1;
    if (p->angle != q->angle)
        return p->angle < q->angle ? -1 : 1;
    return 0;
}

void gx_featset_clear(struct gx_featset *fs)
{
    free(fs->f);
    fs->f = NULL;
    fs->n = 0;
}

int gx_sift_extract(const uint8_t *img8, int w, int h, struct gx_featset *out)
{
    const int L = SIFT_LAYERS, NG = SIFT_LAYERS + 3, ND = SIFT_LAYERS + 2;
    struct fimg *gp = NULL, *dog = NULL;
    struct kp *kps = NULL;
    size_t nkp = 0, cap = 0;
    float sig[SIFT_LAYERS + 3];
    int noct, rc = -1;

    if (!img8 || !out || w < 8 || h < 8)
        return -1;
    out->n = 0;
    out->f = NULL;

    /* number of octaves: based on the upscaled base image (as OpenCV:
     * log2(min) - 2 - firstOctave) */
    noct = cv_round(log((double)(2 * (w < h ? w : h))) / log(2.0) - 2) + 1;
    gp = calloc((size_t)noct * NG, sizeof(*gp));
    dog = calloc((size_t)noct * ND, sizeof(*dog));
    if (!gp || !dog)
        goto out;

    /* incremental σ of each layer */
    sig[0] = SIFT_SIGMA;
    for (int i = 1; i < NG; i++) {
        double k = pow(2.0, 1.0 / L);
        double prev = pow(k, i - 1) * SIFT_SIGMA, total = prev * k;
        sig[i] = (float)sqrt(total * total - prev * prev);
    }

    /* Gaussian pyramid */
    for (int o = 0; o < noct; o++)
        for (int i = 0; i < NG; i++) {
            struct fimg *dst = &gp[o * NG + i];
            if (o == 0 && i == 0) {
                struct fimg up;
                float sd = sqrtf(fmaxf(SIFT_SIGMA * SIFT_SIGMA -
                                       SIFT_INIT_SIGMA * SIFT_INIT_SIGMA * 4, 0.01f));
                if (fimg_alloc(&up, 2 * w, 2 * h) || fimg_alloc(dst, 2 * w, 2 * h)) {
                    free(up.p);
                    goto out;
                }
                upscale2x(img8, w, h, &up);
                rc = gauss(&up, dst, sd);
                free(up.p);
                if (rc)
                    goto out;
                rc = -1;
            } else if (i == 0) {
                const struct fimg *src = &gp[(o - 1) * NG + L];
                if (fimg_alloc(dst, src->w / 2, src->h / 2))
                    goto out;
                downsample(src, dst);
            } else {
                const struct fimg *src = &gp[o * NG + i - 1];
                if (fimg_alloc(dst, src->w, src->h) || gauss(src, dst, sig[i]))
                    goto out;
            }
        }

    /* DoG */
    for (int o = 0; o < noct; o++)
        for (int i = 0; i < ND; i++) {
            const struct fimg *a = &gp[o * NG + i], *b = &gp[o * NG + i + 1];
            struct fimg *d = &dog[o * ND + i];
            if (fimg_alloc(d, a->w, a->h))
                goto out;
            for (int k = 0; k < a->w * a->h; k++)
                d->p[k] = b->p[k] - a->p[k];
        }

    /* extremum detection + orientation */
    {
        const int threshold = (int)floor(0.5 * SIFT_CONTRAST / L * 255);
        for (int o = 0; o < noct; o++) {
            struct fimg *od = &dog[o * ND];
            int ow = od[0].w, oh = od[0].h;
            for (int i = 1; i <= L; i++)
                for (int r = SIFT_IMG_BORDER; r < oh - SIFT_IMG_BORDER; r++)
                    for (int c = SIFT_IMG_BORDER; c < ow - SIFT_IMG_BORDER; c++) {
                        float val = PX(&od[i], c, r);
                        int ismax = val > 0, ismin = val < 0;
                        struct kp k;
                        float hist[SIFT_ORI_BINS], omax, mag_thr;

                        if (!(fabsf(val) > threshold))
                            continue;
                        for (int dl = -1; dl <= 1 && (ismax || ismin); dl++)
                            for (int dy = -1; dy <= 1 && (ismax || ismin); dy++)
                                for (int dx = -1; dx <= 1; dx++) {
                                    float v;
                                    if (!dl && !dy && !dx)
                                        continue;
                                    v = PX(&od[i + dl], c + dx, r + dy);
                                    if (val < v)
                                        ismax = 0;
                                    if (val > v)
                                        ismin = 0;
                                }
                        if (!ismax && !ismin)
                            continue;
                        if (!adjust_extremum(od, o, i, r, c, &k))
                            continue;
                        omax = orientation_hist(&gp[o * NG + k.layer], k.ic, k.ir,
                                                cv_round(SIFT_ORI_RADIUS * k.scl),
                                                SIFT_ORI_SIG_FCTR * k.scl, hist);
                        mag_thr = omax * SIFT_ORI_PEAK;
                        for (int j = 0; j < SIFT_ORI_BINS; j++) {
                            int l = j > 0 ? j - 1 : SIFT_ORI_BINS - 1;
                            int r2 = j < SIFT_ORI_BINS - 1 ? j + 1 : 0;
                            if (hist[j] > hist[l] && hist[j] > hist[r2] && hist[j] >= mag_thr) {
                                float bin = j + 0.5f * (hist[l] - hist[r2]) /
                                            (hist[l] - 2 * hist[j] + hist[r2]);
                                bin = bin < 0 ? SIFT_ORI_BINS + bin :
                                      bin >= SIFT_ORI_BINS ? bin - SIFT_ORI_BINS : bin;
                                k.angle = 360.0f - (360.0f / SIFT_ORI_BINS) * bin;
                                if (fabsf(k.angle - 360.0f) < FLT_EPSILON)
                                    k.angle = 0.0f;
                                if (nkp == cap) {
                                    size_t nc = cap ? cap * 2 : 128;
                                    struct kp *t = realloc(kps, nc * sizeof(*kps));
                                    if (!t)
                                        goto out;
                                    kps = t;
                                    cap = nc;
                                }
                                kps[nkp++] = k;
                            }
                        }
                    }
        }
    }

    /* deduplication (as removeDuplicatedSorted) */
    if (nkp > 1) {
        size_t m = 1;
        qsort(kps, nkp, sizeof(*kps), kp_cmp);
        for (size_t i = 1; i < nkp; i++) {
            const struct kp *p = &kps[m - 1], *q = &kps[i];
            if (p->ox != q->ox || p->oy != q->oy || p->size != q->size || p->angle != q->angle)
                kps[m++] = *q;
        }
        nkp = m;
    }

    /* descriptors */
    out->f = calloc(nkp ? nkp : 1, sizeof(*out->f));
    if (!out->f)
        goto out;
    for (size_t i = 0; i < nkp; i++) {
        const struct kp *k = &kps[i];
        float ori = 360.0f - k->angle;
        if (fabsf(ori - 360.0f) < FLT_EPSILON)
            ori = 0.0f;
        descriptor(&gp[k->o * NG + k->layer], k->x, k->y, ori, k->scl, out->f[i].desc);
        out->f[i].x = k->ox;
        out->f[i].y = k->oy;
    }
    out->n = (uint32_t)nkp;
    rc = 0;

out:
    if (gp)
        for (int i = 0; i < noct * NG; i++)
            free(gp[i].p);
    if (dog)
        for (int i = 0; i < noct * ND; i++)
            free(dog[i].p);
    free(gp);
    free(dog);
    free(kps);
    if (rc)
        gx_featset_clear(out);
    return rc;
}
