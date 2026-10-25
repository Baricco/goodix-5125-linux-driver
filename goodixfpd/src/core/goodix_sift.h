/* goodix_sift.h - 无 OpenCV 依赖的 SIFT 特征提取（主机侧比对用）
 *
 * 与 OpenCV cv::SIFT 默认参数逐项一致（nOctaveLayers=3,
 * contrastThreshold=0.04, edgeThreshold=10, sigma=1.6, 首组上采样 2x），
 * 输出关键点坐标（原图像素）与 128 维描述子（每维 0..255）。
 * 纯 C、无全局状态、可重入。
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GOODIX_SIFT_H
#define GOODIX_SIFT_H

#include <stdint.h>

#define GX_SIFT_DESC_LEN 128

/* 一个特征点：位置（原图坐标）+ 描述子 */
struct gx_feat {
    float x, y;
    uint8_t desc[GX_SIFT_DESC_LEN];
};

/* 一帧的特征集合 */
struct gx_featset {
    uint32_t n;
    struct gx_feat *f;      /* malloc 分配，gx_featset_clear 释放 */
};

/* 从 8bit 灰度图（w*h，行优先）提取 SIFT 特征。
 * 返回 0 成功（out->n 可为 0）；-1 参数非法或分配失败。 */
int gx_sift_extract(const uint8_t *img8, int w, int h, struct gx_featset *out);

void gx_featset_clear(struct gx_featset *fs);

#endif /* GOODIX_SIFT_H */
