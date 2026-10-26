/* goodix_match.h - SIGFM geometric-consistency matching (plain C) + template
 * serialization
 *
 * Same matching algorithm as SIGFM (goodix-fp-linux-dev / libfprint MR !530):
 * 2-NN ratio test -> pairwise length consistency -> rotation-angle consistency
 * voting, score = number of pairs with consistent angles. Two extra fixes:
 * correspondences displaced by <= 2 px are dropped (sensor fixed pattern), and
 * deduplication uses a full (p1, p2) ordering.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GOODIX_MATCH_H
#define GOODIX_MATCH_H

#include <stddef.h>
#include <stdint.h>
#include "goodix_sift.h"

/* Score of two feature sets (>= 0); 0 = no match. Every keypoint of query
 * gets a 2-NN ratio test in train, so the result is asymmetric. Usage
 * consistent with libfprint: query = enrolled frame, train = new capture
 * (fpi_print_sigfm_match calls sigfm_match_score(template frame, capture)). */
int gx_match_score(const struct gx_featset *query, const struct gx_featset *train);

/* Serialization: [u32 magic][u32 n] + n * [f32 x][f32 y][128 B desc], little
 * endian. Returns a malloc'ed buffer (*len = size in bytes), NULL on failure. */
uint8_t *gx_featset_serialize(const struct gx_featset *fs, size_t *len);
/* Deserialization, checks length and magic. Returns 0 on success. */
int gx_featset_deserialize(const uint8_t *buf, size_t len, struct gx_featset *out);

#endif /* GOODIX_MATCH_H */
