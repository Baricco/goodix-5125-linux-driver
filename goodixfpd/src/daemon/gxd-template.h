/* gxd-template.h - enrolled fingerprint: a set of SIFT frames of one finger.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <glib.h>

#include "goodix_sift.h"

/* Matching rules identical to SIGFM in libfprint with the goodixgf driver:
 * it is a match if at least one enrolled frame scores >= threshold. */
#define GXD_MATCH_THRESHOLD   20
/* A frame with fewer keypoints is neither enrolled nor matched
 * (in libfprint: "No enough keypoints found" -> retry). */
#define GXD_MIN_KEYPOINTS     25

typedef enum {
  GXD_FRAME_ENROLLED = 0,     /* captured during enrollment */
  GXD_FRAME_LEARNED  = 1,     /* added by learning during verification */
} GxdFrameOrigin;

typedef struct {
  struct gx_featset fs;
  guint8            origin;   /* GxdFrameOrigin */
  guint32           hits;     /* verifications decided by this frame */
  gint64            added;    /* unix time */
  gint64            last_used;
} GxdFrame;

typedef struct {
  gint       ref;
  char      *username;
  int        finger;
  gint64     enrolled;        /* unix time */
  GPtrArray *frames;          /* GxdFrame* */
} GxdTemplate;

typedef struct {
  gboolean matched;
  int      best_score;        /* maximum over all frames */
  int      best_frame;        /* index of the best frame, -1 if none */
  int      frames_matched;    /* frames scoring >= threshold */
} GxdMatch;

GxdTemplate *gxd_template_new (const char *username, int finger);
GxdTemplate *gxd_template_ref (GxdTemplate *tpl);
void         gxd_template_unref (GxdTemplate *tpl);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GxdTemplate, gxd_template_unref)

/* Adds a frame, copying its features. */
void gxd_template_add (GxdTemplate *tpl, const struct gx_featset *fs,
                       GxdFrameOrigin origin, gint64 now);

/* Matches a capture against all frames. */
void gxd_template_match (const GxdTemplate *tpl, const struct gx_featset *probe,
                         GxdMatch *out);

/* Learning during verification (like identifyUpdate/templateStudy of the
 * Windows driver): a verification that succeeds with margin in a poorly
 * covered area adds the capture as a learned frame, so the coverage of the
 * finger grows with use. Enrollment frames are never replaced; beyond the
 * capacity the oldest learned frame goes.
 * Returns TRUE if the fingerprint changed (it must be saved). */
#define GXD_LEARN_MIN_SCORE    40   /* 2x the acceptance threshold */
#define GXD_LEARN_MAX_OVERLAP   3   /* frames that already recognize the capture */
#define GXD_LEARN_CAPACITY     30   /* maximum number of learned frames */

gboolean gxd_template_learn (GxdTemplate *tpl, const struct gx_featset *probe,
                             const GxdMatch *m, gint64 now);

/* On-disk format (versioned). NULL if the data is not valid. */
GBytes      *gxd_template_serialize (const GxdTemplate *tpl);
GxdTemplate *gxd_template_deserialize (GBytes *data, GError **error);

void gxd_featset_copy (struct gx_featset *dst, const struct gx_featset *src);
