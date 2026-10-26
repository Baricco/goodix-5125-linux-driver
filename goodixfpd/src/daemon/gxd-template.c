/* gxd-template.c - enrolled fingerprint: matching and on-disk format.
 *
 * File format: "GXFPD1\n" followed by a GVariant in normal form
 *   (u version, s username, y finger, x enrolled,
 *    a(yuxxay) frames: origin, hits, added, last_used, serialized features)
 * The features use the core's gx_featset_serialize().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>

#include "gxd.h"
#include "gxd-template.h"
#include "goodix_match.h"

#define FILE_MAGIC     "GXFPD1\n"
#define FILE_VERSION   1
#define FILE_TYPE      "(usyxa(yuxxay))"
#define MAX_FRAMES     256

void
gxd_featset_copy (struct gx_featset *dst, const struct gx_featset *src)
{
  dst->n = src->n;
  dst->f = g_memdup2 (src->f, (gsize) src->n * sizeof (*src->f));
}

static void
frame_free (gpointer data)
{
  GxdFrame *fr = data;

  gx_featset_clear (&fr->fs);
  g_free (fr);
}

GxdTemplate *
gxd_template_new (const char *username, int finger)
{
  GxdTemplate *tpl = g_new0 (GxdTemplate, 1);

  tpl->ref = 1;
  tpl->username = g_strdup (username);
  tpl->finger = finger;
  tpl->frames = g_ptr_array_new_with_free_func (frame_free);
  return tpl;
}

GxdTemplate *
gxd_template_ref (GxdTemplate *tpl)
{
  g_atomic_int_inc (&tpl->ref);
  return tpl;
}

void
gxd_template_unref (GxdTemplate *tpl)
{
  if (tpl && g_atomic_int_dec_and_test (&tpl->ref))
    {
      g_ptr_array_unref (tpl->frames);
      g_free (tpl->username);
      g_free (tpl);
    }
}

void
gxd_template_add (GxdTemplate *tpl, const struct gx_featset *fs,
                  GxdFrameOrigin origin, gint64 now)
{
  GxdFrame *fr = g_new0 (GxdFrame, 1);

  gxd_featset_copy (&fr->fs, fs);
  fr->origin = origin;
  fr->added = now;
  g_ptr_array_add (tpl->frames, fr);
}

void
gxd_template_match (const GxdTemplate *tpl, const struct gx_featset *probe,
                    GxdMatch *out)
{
  memset (out, 0, sizeof (*out));
  out->best_frame = -1;

  for (guint i = 0; i < tpl->frames->len; i++)
    {
      const GxdFrame *fr = g_ptr_array_index (tpl->frames, i);
      /* Same order as libfprint (sigfm_match_score(enrolled frame, capture)):
       * the 2-NN ratio test is not symmetric and with the reverse order the
       * scores change. */
      int score = gx_match_score (&fr->fs, probe);

      if (score > out->best_score)
        {
          out->best_score = score;
          out->best_frame = (int) i;
        }
      if (score >= GXD_MATCH_THRESHOLD)
        out->frames_matched++;
    }
  out->matched = out->best_score >= GXD_MATCH_THRESHOLD;
}

gboolean
gxd_template_learn (GxdTemplate *tpl, const struct gx_featset *probe,
                    const GxdMatch *m, gint64 now)
{
  GxdFrame *best;
  guint learned = 0;
  int oldest = -1;

  if (!m->matched || m->best_score < GXD_LEARN_MIN_SCORE ||
      m->frames_matched > GXD_LEARN_MAX_OVERLAP || probe->n < GXD_MIN_KEYPOINTS)
    return FALSE;

  best = g_ptr_array_index (tpl->frames, m->best_frame);
  best->hits++;
  best->last_used = now;
  gxd_template_add (tpl, probe, GXD_FRAME_LEARNED, now);

  for (guint i = 0; i < tpl->frames->len; i++)
    {
      const GxdFrame *fr = g_ptr_array_index (tpl->frames, i);

      if (fr->origin != GXD_FRAME_LEARNED)
        continue;
      learned++;
      if (oldest < 0 ||
          fr->added < ((GxdFrame *) g_ptr_array_index (tpl->frames, oldest))->added)
        oldest = (int) i;
    }
  if (learned > GXD_LEARN_CAPACITY)
    g_ptr_array_remove_index (tpl->frames, oldest);
  return TRUE;
}

GBytes *
gxd_template_serialize (const GxdTemplate *tpl)
{
  g_autoptr(GVariant) v = NULL;
  g_autoptr(GBytes) body = NULL;
  GVariantBuilder frames;
  GByteArray *out;

  g_variant_builder_init (&frames, G_VARIANT_TYPE ("a(yuxxay)"));
  for (guint i = 0; i < tpl->frames->len; i++)
    {
      const GxdFrame *fr = g_ptr_array_index (tpl->frames, i);
      size_t len = 0;
      g_autofree uint8_t *buf = gx_featset_serialize (&fr->fs, &len);

      if (!buf)
        return NULL;
      g_variant_builder_add (&frames, "(yuxx@ay)", fr->origin, fr->hits,
                             fr->added, fr->last_used,
                             g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                        buf, len, 1));
    }
  v = g_variant_ref_sink (g_variant_new (FILE_TYPE, (guint32) FILE_VERSION,
                                         tpl->username, (guchar) tpl->finger,
                                         tpl->enrolled, &frames));
  body = g_variant_get_data_as_bytes (v);

  out = g_byte_array_new ();
  g_byte_array_append (out, (const guint8 *) FILE_MAGIC, strlen (FILE_MAGIC));
  g_byte_array_append (out, g_bytes_get_data (body, NULL), g_bytes_get_size (body));
  return g_byte_array_free_to_bytes (out);
}

GxdTemplate *
gxd_template_deserialize (GBytes *data, GError **error)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(GVariant) v = NULL;
  g_autoptr(GVariantIter) iter = NULL;
  g_autoptr(GxdTemplate) tpl = NULL;
  const char *username;
  guint32 version;
  guchar finger;
  gint64 enrolled;
  gsize size;
  const guint8 *raw = g_bytes_get_data (data, &size);
  guchar origin;
  guint32 hits;
  gint64 added, last_used;
  GVariant *feats;

  if (size < strlen (FILE_MAGIC) || memcmp (raw, FILE_MAGIC, strlen (FILE_MAGIC)) != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "not a goodixfpd print");
      return NULL;
    }
  body = g_bytes_new_from_bytes (data, strlen (FILE_MAGIC), size - strlen (FILE_MAGIC));
  v = g_variant_ref_sink (g_variant_new_from_bytes (G_VARIANT_TYPE (FILE_TYPE), body, FALSE));
  if (!g_variant_is_normal_form (v))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "corrupted print data");
      return NULL;
    }

  g_variant_get (v, "(u&syxa(yuxxay))", &version, &username, &finger, &enrolled, &iter);
  if (version != FILE_VERSION || finger < GXD_FINGER_FIRST || finger > GXD_FINGER_LAST ||
      g_variant_iter_n_children (iter) > MAX_FRAMES)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "unsupported print (version %u, finger %u)", version, finger);
      return NULL;
    }

  tpl = gxd_template_new (username, finger);
  tpl->enrolled = enrolled;
  while (g_variant_iter_next (iter, "(yuxx@ay)", &origin, &hits, &added, &last_used, &feats))
    {
      g_autoptr(GVariant) owned = feats;
      gsize flen;
      const guint8 *fbuf = g_variant_get_fixed_array (feats, &flen, 1);
      GxdFrame *fr = g_new0 (GxdFrame, 1);

      if (gx_featset_deserialize (fbuf, flen, &fr->fs) != 0)
        {
          g_free (fr);
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "corrupted feature set");
          return NULL;
        }
      fr->origin = origin;
      fr->hits = hits;
      fr->added = added;
      fr->last_used = last_used;
      g_ptr_array_add (tpl->frames, fr);
    }

  return g_steal_pointer (&tpl);
}
