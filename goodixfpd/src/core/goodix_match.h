/* goodix_match.h - SIGFM 几何一致性比对（纯 C）+ 模板序列化
 *
 * 比对算法同 SIGFM（goodix-fp-linux-dev / libfprint MR !530）：
 * 2-NN 比值检验 -> 成对长度一致性 -> 旋转角一致性投票，得分 = 角度一致的
 * 对的对数。另加两项修正：丢弃位移 <= 2px 的对应（传感器固定图案），
 * 去重按 (p1, p2) 完整排序。
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GOODIX_MATCH_H
#define GOODIX_MATCH_H

#include <stddef.h>
#include <stdint.h>
#include "goodix_sift.h"

/* 两个特征集的得分（>=0）；0 = 不匹配。query 的每个关键点在 train 中做
 * 2-NN 比值检验，结果不对称。与 libfprint 一致的用法：query = 已注册帧，
 * train = 新采集帧（fpi_print_sigfm_match 调用 sigfm_match_score(模板帧, 采集)）。 */
int gx_match_score(const struct gx_featset *query, const struct gx_featset *train);

/* 序列化：[u32 magic][u32 n] + n * [f32 x][f32 y][128 B desc]，小端。
 * 返回 malloc 缓冲（*len 为字节数），失败 NULL。 */
uint8_t *gx_featset_serialize(const struct gx_featset *fs, size_t *len);
/* 反序列化，校验长度与 magic。返回 0 成功。 */
int gx_featset_deserialize(const uint8_t *buf, size_t len, struct gx_featset *out);

#endif /* GOODIX_MATCH_H */
