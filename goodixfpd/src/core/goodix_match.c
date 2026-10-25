/* goodix_match.c - SIGFM 比对的纯 C 实现（语义同 sigfm_match_score）
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "goodix_match.h"

#define DISTANCE_MATCH   0.75   /* Lowe 比值检验 */
#define LENGTH_MATCH     0.05   /* 成对长度相对误差 */
#define ANGLE_MATCH      0.05   /* 旋转角编码相对误差 */
#define MIN_MATCH        5
/* 两端关键点几乎在同一传感器像素上的对应来自固定图案（逐像素增益/基线
 * 残影），跨手指也会以完美几何一致匹配；真实的两次按压不会亚像素重合。 */
#define MIN_DISPLACEMENT 2.0

#define FEATSET_MAGIC    0x31534647u   /* "GFS1" */
#define HALF_PI          1.57079632679489661923
#define FEAT_REC_LEN     (8 + GX_SIFT_DESC_LEN)

struct corr {
    int x1, y1, x2, y2;     /* cvRound 后的坐标（同 cv::Point2i） */
};

struct angle {
    double c, s;
};

static int corr_cmp(const void *a, const void *b)
{
    const struct corr *p = a, *q = b;
    if (p->y1 != q->y1) return p->y1 < q->y1 ? -1 : 1;
    if (p->x1 != q->x1) return p->x1 < q->x1 ? -1 : 1;
    if (p->y2 != q->y2) return p->y2 < q->y2 ? -1 : 1;
    if (p->x2 != q->x2) return p->x2 < q->x2 ? -1 : 1;
    return 0;
}

static float desc_dist(const uint8_t *a, const uint8_t *b)
{
    int s = 0;
    for (int i = 0; i < GX_SIFT_DESC_LEN; i++) {
        int d = (int)a[i] - (int)b[i];
        s += d * d;
    }
    return sqrtf((float)s);
}

/* 相对误差 1 - min/max <= tol；含 NaN 时为假（同原 C++ 浮点语义） */
static inline int rel_close(double a, double b, double tol)
{
    double mn = a < b ? a : b, mx = a < b ? b : a;
    return 1.0 - mn / mx <= tol;
}

int gx_match_score(const struct gx_featset *query, const struct gx_featset *train)
{
    struct corr *m = NULL;
    struct angle *ang = NULL;
    size_t nm = 0, nu = 0, na = 0;
    int nb_matched = 0, count = 0;

    if (!query || !train || query->n == 0 || train->n < 2)
        return 0;
    m = malloc(query->n * sizeof(*m));
    if (!m)
        return -1;

    /* 2-NN + 比值检验 + 位移过滤 */
    for (uint32_t q = 0; q < query->n; q++) {
        float d1 = INFINITY, d2 = INFINITY;
        uint32_t best = 0;
        for (uint32_t t = 0; t < train->n; t++) {
            float d = desc_dist(query->f[q].desc, train->f[t].desc);
            if (d < d1) {
                d2 = d1;
                d1 = d;
                best = t;
            } else if (d < d2) {
                d2 = d;
            }
        }
        if (!(d1 < DISTANCE_MATCH * d2))
            continue;
        {
            const struct gx_feat *a = &query->f[q], *b = &train->f[best];
            double dx = a->x - b->x, dy = a->y - b->y;
            if (sqrt(dx * dx + dy * dy) <= MIN_DISPLACEMENT)
                continue;
            m[nm].x1 = (int)lrintf(a->x);
            m[nm].y1 = (int)lrintf(a->y);
            m[nm].x2 = (int)lrintf(b->x);
            m[nm].y2 = (int)lrintf(b->y);
            nm++;
            nb_matched++;
        }
    }
    if (nb_matched < MIN_MATCH)
        goto out;

    /* 去重（std::set 语义） */
    qsort(m, nm, sizeof(*m), corr_cmp);
    for (size_t i = 0; i < nm; i++)
        if (nu == 0 || corr_cmp(&m[nu - 1], &m[i]) != 0)
            m[nu++] = m[i];

    /* 成对长度一致 -> 旋转角编码 */
    ang = malloc((nu * (nu - 1) / 2 + 1) * sizeof(*ang));
    if (!ang) {
        count = -1;
        goto out;
    }
    for (size_t j = 0; j < nu; j++)
        for (size_t k = j + 1; k < nu; k++) {
            int v1x = m[j].x1 - m[k].x1, v1y = m[j].y1 - m[k].y1;
            int v2x = m[j].x2 - m[k].x2, v2y = m[j].y2 - m[k].y2;
            double l1 = sqrt((double)v1x * v1x + (double)v1y * v1y);
            double l2 = sqrt((double)v2x * v2x + (double)v2y * v2y);
            if (rel_close(l1, l2, LENGTH_MATCH)) {
                double p = l1 * l2;
                ang[na].c = HALF_PI + asin((v1x * v2x + v1y * v2y) / p);
                ang[na].s = acos((v1x * v2y - v1y * v2x) / p);
                na++;
            }
        }
    if (na < MIN_MATCH)
        goto out;

    /* 角度一致的对计数 */
    for (size_t j = 0; j < na; j++)
        for (size_t k = j + 1; k < na; k++)
            if (rel_close(ang[j].s, ang[k].s, ANGLE_MATCH) &&
                rel_close(ang[j].c, ang[k].c, ANGLE_MATCH))
                count++;

out:
    free(m);
    free(ang);
    return count;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint8_t *gx_featset_serialize(const struct gx_featset *fs, size_t *len)
{
    size_t total = 8 + (size_t)fs->n * FEAT_REC_LEN;
    uint8_t *buf = malloc(total), *p;

    if (!buf)
        return NULL;
    put_u32(buf, FEATSET_MAGIC);
    put_u32(buf + 4, fs->n);
    p = buf + 8;
    for (uint32_t i = 0; i < fs->n; i++, p += FEAT_REC_LEN) {
        uint32_t xb, yb;
        memcpy(&xb, &fs->f[i].x, 4);
        memcpy(&yb, &fs->f[i].y, 4);
        put_u32(p, xb);
        put_u32(p + 4, yb);
        memcpy(p + 8, fs->f[i].desc, GX_SIFT_DESC_LEN);
    }
    *len = total;
    return buf;
}

int gx_featset_deserialize(const uint8_t *buf, size_t len, struct gx_featset *out)
{
    uint32_t n;
    const uint8_t *p;

    out->n = 0;
    out->f = NULL;
    if (!buf || len < 8 || get_u32(buf) != FEATSET_MAGIC)
        return -1;
    n = get_u32(buf + 4);
    if (n > 100000 || len != 8 + (size_t)n * FEAT_REC_LEN)
        return -1;
    out->f = calloc(n ? n : 1, sizeof(*out->f));
    if (!out->f)
        return -1;
    p = buf + 8;
    for (uint32_t i = 0; i < n; i++, p += FEAT_REC_LEN) {
        uint32_t xb = get_u32(p), yb = get_u32(p + 4);
        memcpy(&out->f[i].x, &xb, 4);
        memcpy(&out->f[i].y, &yb, 4);
        memcpy(out->f[i].desc, p + 8, GX_SIFT_DESC_LEN);
    }
    out->n = n;
    return 0;
}
