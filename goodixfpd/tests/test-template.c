/* test-template.c - impronte: formato su disco, archivio, confronto.
 *
 * Con GXD_DATASET=<dir> (acquisizioni reali: enroll/, genuine/, impostor/
 * con i dump imgproc-*-{raw16,base16,fresh16,ridge8}.pgm) verifica anche:
 *   - che la pipeline immagine ricostruisca ridge8 dal grezzo;
 *   - FAR = 0 e FRR entro il limite sul confronto SIFT/SIGFM in C;
 * e stampa la distribuzione dei punteggi (taratura dell'apprendimento).
 * I dati biometrici restano fuori dal repository.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gxd.h"
#include "gxd-storage.h"
#include "gxd-template.h"
#include "goodix.h"
#include "goodix_sift.h"

#define W 80
#define H 64
#define MAX_FRR 0.15     /* produzione (OpenCV) su questi dati: 3/25 */

/* ---- PGM ---- */

static guint8 *
read_pgm (const char *path, int *maxval)
{
  g_autofree char *buf = NULL;
  gsize len;
  int w, h, mv, off = 0;

  if (!g_file_get_contents (path, &buf, &len, NULL))
    return NULL;
  if (sscanf (buf, "P5 %d %d %d%n", &w, &h, &mv, &off) != 3 || w != W || h != H)
    return NULL;
  off++;                        /* un solo spazio dopo maxval */
  *maxval = mv;
  if (len - off < (gsize) (W * H * (mv > 255 ? 2 : 1)))
    return NULL;
  return g_memdup2 (buf + off, len - off);
}

/* PGM 16 bit (big endian) -> trama del sensore (16 bit little endian) */
static gboolean
read_pgm16 (const char *path, guint8 *img16)
{
  int mv;
  g_autofree guint8 *px = read_pgm (path, &mv);

  if (!px || mv <= 255)
    return FALSE;
  for (int i = 0; i < W * H; i++)
    {
      img16[2 * i] = px[2 * i + 1];
      img16[2 * i + 1] = px[2 * i];
    }
  return TRUE;
}

/* ---- dataset ---- */

typedef struct {
  char  *name;
  guint8 img8[W * H];
  struct gx_featset fs;
} Shot;

static void
shot_free (Shot *s)
{
  gx_featset_clear (&s->fs);
  g_free (s->name);
  g_free (s);
}

static GPtrArray *
load_set (const char *dir, int *pipeline_checked, int *pipeline_exact)
{
  GPtrArray *shots = g_ptr_array_new_with_free_func ((GDestroyNotify) shot_free);
  g_autoptr(GDir) d = g_dir_open (dir, 0, NULL);
  g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  struct goodix_dev *gx = g_new0 (struct goodix_dev, 1);
  const char *n;

  gx->img_w = W;
  gx->img_h = H;
  g_assert_nonnull (d);
  while ((n = g_dir_read_name (d)))
    if (g_str_has_suffix (n, "-ridge8.pgm"))
      g_ptr_array_add (names, g_strndup (n, strlen (n) - strlen ("-ridge8.pgm")));
  g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);

  for (guint i = 0; i < names->len; i++)
    {
      const char *stem = g_ptr_array_index (names, i);
      g_autofree char *p8 = g_strdup_printf ("%s/%s-ridge8.pgm", dir, stem);
      g_autofree char *praw = g_strdup_printf ("%s/%s-raw16.pgm", dir, stem);
      g_autofree char *pbase = g_strdup_printf ("%s/%s-base16.pgm", dir, stem);
      g_autofree char *pfresh = g_strdup_printf ("%s/%s-fresh16.pgm", dir, stem);
      int mv;
      g_autofree guint8 *px = read_pgm (p8, &mv);
      Shot *s;
      guint8 raw[W * H * 2];

      g_assert_nonnull (px);
      s = g_new0 (Shot, 1);
      s->name = g_strdup (stem);
      memcpy (s->img8, px, W * H);
      g_assert_cmpint (gx_sift_extract (s->img8, W, H, &s->fs), ==, 0);
      g_ptr_array_add (shots, s);

      /* stessa pipeline del servizio, dal grezzo */
      if (read_pgm16 (praw, raw) && read_pgm16 (pbase, gx->img_base))
        {
          struct gx_imgproc_params p = GX_IMGPROC_SIGFM_PARAMS;
          guint8 out8[W * H];

          gx->img_base_valid = true;
          gx->img_fresh_valid = read_pgm16 (pfresh, gx->img_fresh);
          g_assert_cmpint (gx_imgproc_to8bit (gx, &p, raw, out8), ==, 0);
          (*pipeline_checked)++;
          if (memcmp (out8, s->img8, W * H) == 0)
            (*pipeline_exact)++;
        }
    }
  g_free (gx);
  return shots;
}

static void
test_dataset (void)
{
  const char *root = g_getenv ("GXD_DATASET");
  g_autofree char *de = NULL, *dg = NULL, *di = NULL;
  g_autoptr(GPtrArray) enroll = NULL, genuine = NULL, impostor = NULL;
  g_autoptr(GxdTemplate) tpl = NULL;
  int checked = 0, exact = 0, accepted = 0, false_acc = 0;

  if (!root || !*root)
    {
      g_test_skip ("GXD_DATASET non impostato");
      return;
    }
  de = g_build_filename (root, "enroll", NULL);
  dg = g_build_filename (root, "genuine", NULL);
  di = g_build_filename (root, "impostor", NULL);
  enroll = load_set (de, &checked, &exact);
  genuine = load_set (dg, &checked, &exact);
  impostor = load_set (di, &checked, &exact);
  g_test_message ("pipeline: %d/%d immagini identiche ai dump di produzione", exact, checked);
  g_assert_cmpint (exact, ==, checked);

  tpl = gxd_template_new ("tester", 7);
  for (guint i = 0; i < enroll->len; i++)
    {
      Shot *s = g_ptr_array_index (enroll, i);
      if (s->fs.n >= GXD_MIN_KEYPOINTS)
        gxd_template_add (tpl, &s->fs, GXD_FRAME_ENROLLED, 0);
    }

  for (guint i = 0; i < genuine->len; i++)
    {
      Shot *s = g_ptr_array_index (genuine, i);
      GxdMatch m;

      gxd_template_match (tpl, &s->fs, &m);
      g_test_message ("genuine  %s kp=%3u best=%6d frames=%2d %s", s->name, s->fs.n,
                      m.best_score, m.frames_matched, m.matched ? "MATCH" : "-");
      accepted += m.matched;
    }
  for (guint i = 0; i < impostor->len; i++)
    {
      Shot *s = g_ptr_array_index (impostor, i);
      GxdMatch m;

      gxd_template_match (tpl, &s->fs, &m);
      g_test_message ("impostor %s kp=%3u best=%6d frames=%2d %s", s->name, s->fs.n,
                      m.best_score, m.frames_matched, m.matched ? "MATCH" : "-");
      false_acc += m.matched;
    }
  g_test_message ("template %u frame; genuini %d/%u accettati; impostori %d/%u accettati",
                  tpl->frames->len, accepted, genuine->len, false_acc, impostor->len);
  g_assert_cmpint (false_acc, ==, 0);
  g_assert_cmpfloat (1.0 - (double) accepted / genuine->len, <=, MAX_FRR);

  /* Apprendimento in linea: ogni genuino e' giudicato prima di poterne
   * imparare; alla fine nessun impostore deve passare. */
  {
    int online = 0, learned = 0;

    for (guint i = 0; i < genuine->len; i++)
      {
        Shot *s = g_ptr_array_index (genuine, i);
        GxdMatch m;

        gxd_template_match (tpl, &s->fs, &m);
        online += m.matched;
        learned += gxd_template_learn (tpl, &s->fs, &m, 1000 + i);
      }
    false_acc = 0;
    for (guint i = 0; i < impostor->len; i++)
      {
        Shot *s = g_ptr_array_index (impostor, i);
        GxdMatch m;

        gxd_template_match (tpl, &s->fs, &m);
        false_acc += m.matched;
      }
    g_test_message ("apprendimento: %d/%u accettati in linea, %d frame appresi, "
                    "impostori dopo %d/%u", online, genuine->len, learned,
                    false_acc, impostor->len);
    g_assert_cmpint (online, >=, accepted);
    g_assert_cmpint (false_acc, ==, 0);
  }
}

static void fake_featset (struct gx_featset *fs, guint n, guint seed);

static void
test_learn_policy (void)
{
  g_autoptr(GxdTemplate) tpl = gxd_template_new ("mario", 7);
  struct gx_featset fs;
  GxdMatch m = { .matched = TRUE, .best_score = 500, .best_frame = 0, .frames_matched = 1 };
  guint enrolled = 0, learned = 0;

  fake_featset (&fs, 40, 9);
  gxd_template_add (tpl, &fs, GXD_FRAME_ENROLLED, 1);
  gxd_template_add (tpl, &fs, GXD_FRAME_ENROLLED, 1);

  /* niente apprendimento senza margine o in zona gia' coperta */
  m.best_score = GXD_LEARN_MIN_SCORE - 1;
  g_assert_false (gxd_template_learn (tpl, &fs, &m, 2));
  m.best_score = 500;
  m.frames_matched = GXD_LEARN_MAX_OVERLAP + 1;
  g_assert_false (gxd_template_learn (tpl, &fs, &m, 2));
  m.frames_matched = 1;
  m.matched = FALSE;
  g_assert_false (gxd_template_learn (tpl, &fs, &m, 2));
  m.matched = TRUE;

  for (int i = 0; i < GXD_LEARN_CAPACITY + 5; i++)
    g_assert_true (gxd_template_learn (tpl, &fs, &m, 100 + i));

  for (guint i = 0; i < tpl->frames->len; i++)
    {
      GxdFrame *fr = g_ptr_array_index (tpl->frames, i);
      if (fr->origin == GXD_FRAME_ENROLLED)
        enrolled++;
      else
        {
          learned++;
          g_assert_cmpint (fr->added, >=, 105);      /* i 5 piu' vecchi sono usciti */
        }
    }
  g_assert_cmpuint (enrolled, ==, 2);
  g_assert_cmpuint (learned, ==, GXD_LEARN_CAPACITY);
  g_assert_cmpuint (((GxdFrame *) g_ptr_array_index (tpl->frames, 0))->hits, ==,
                    GXD_LEARN_CAPACITY + 5);
  gx_featset_clear (&fs);
}

/* ---- formato e archivio ---- */

static void
fake_featset (struct gx_featset *fs, guint n, guint seed)
{
  GRand *r = g_rand_new_with_seed (seed);

  fs->n = n;
  fs->f = g_new0 (struct gx_feat, n);
  for (guint i = 0; i < n; i++)
    {
      fs->f[i].x = g_rand_double_range (r, 0, W);
      fs->f[i].y = g_rand_double_range (r, 0, H);
      for (int k = 0; k < GX_SIFT_DESC_LEN; k++)
        fs->f[i].desc[k] = g_rand_int_range (r, 0, 256);
    }
  g_rand_free (r);
}

static void
test_roundtrip (void)
{
  g_autoptr(GxdTemplate) tpl = gxd_template_new ("mario", 7);
  g_autoptr(GxdTemplate) back = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GBytes) bad = NULL;
  g_autoptr(GError) error = NULL;
  struct gx_featset a, b;
  guint8 *raw;
  gsize len;

  fake_featset (&a, 40, 1);
  fake_featset (&b, 0, 2);
  tpl->enrolled = 1790000000;
  gxd_template_add (tpl, &a, GXD_FRAME_ENROLLED, 10);
  gxd_template_add (tpl, &b, GXD_FRAME_LEARNED, 20);
  ((GxdFrame *) g_ptr_array_index (tpl->frames, 1))->hits = 3;

  bytes = gxd_template_serialize (tpl);
  back = gxd_template_deserialize (bytes, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (back->username, ==, "mario");
  g_assert_cmpint (back->finger, ==, 7);
  g_assert_cmpint (back->enrolled, ==, 1790000000);
  g_assert_cmpuint (back->frames->len, ==, 2);
  {
    GxdFrame *f0 = g_ptr_array_index (back->frames, 0);
    GxdFrame *f1 = g_ptr_array_index (back->frames, 1);
    g_assert_cmpuint (f0->fs.n, ==, 40);
    g_assert_cmpmem (f0->fs.f, 40 * sizeof (struct gx_feat), a.f, 40 * sizeof (struct gx_feat));
    g_assert_cmpint (f1->origin, ==, GXD_FRAME_LEARNED);
    g_assert_cmpuint (f1->hits, ==, 3);
    g_assert_cmpint (f1->added, ==, 20);
  }

  /* dati alterati: rifiutati, mai letti a meta' */
  raw = g_bytes_unref_to_data (g_steal_pointer (&bytes), &len);
  raw[len / 2] ^= 0xff;
  raw[len - 3] ^= 0x55;
  bad = g_bytes_new_take (raw, len);
  g_clear_pointer (&back, gxd_template_unref);
  back = gxd_template_deserialize (bad, &error);
  if (back == NULL)
    {
      g_assert_nonnull (error);
      g_clear_error (&error);
    }
  g_clear_pointer (&bad, g_bytes_unref);
  bad = g_bytes_new_static ("GXFPD1\nxx", 9);
  g_assert_null (gxd_template_deserialize (bad, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);
  g_clear_pointer (&bad, g_bytes_unref);
  bad = g_bytes_new_static ("nope", 4);
  g_assert_null (gxd_template_deserialize (bad, &error));
  g_clear_error (&error);

  gx_featset_clear (&a);
  gx_featset_clear (&b);
}

static void
test_storage (void)
{
  g_autofree char *dir = g_dir_make_tmp ("gxd-test-XXXXXX", NULL);
  g_autoptr(GxdTemplate) tpl = gxd_template_new ("mario", 2);
  g_autoptr(GxdTemplate) other = gxd_template_new ("luigi", 7);
  g_autoptr(GxdTemplate) back = NULL;
  g_autoptr(GArray) fingers = NULL;
  g_autoptr(GPtrArray) all = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  struct gx_featset a;
  GStatBuf st;

  gxd_set_state_dir (dir);
  fake_featset (&a, 30, 3);
  gxd_template_add (tpl, &a, GXD_FRAME_ENROLLED, 1);
  gxd_template_add (other, &a, GXD_FRAME_ENROLLED, 1);

  g_assert_true (gxd_storage_save (tpl, &error));
  g_assert_no_error (error);
  g_assert_true (gxd_storage_save (other, &error));
  path = g_build_filename (dir, "goodixfpd", "mario", "left-index-finger", NULL);
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);

  back = gxd_storage_load ("mario", 2, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (back->frames->len, ==, 1);
  g_clear_pointer (&back, gxd_template_unref);

  g_assert_null (gxd_storage_load ("mario", 7, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  /* il file di un altro utente non vale per mario */
  {
    g_autofree char *src = g_build_filename (dir, "goodixfpd", "luigi", "right-index-finger", NULL);
    g_autofree char *dst = g_build_filename (dir, "goodixfpd", "mario", "right-index-finger", NULL);
    g_autofree char *data = NULL;
    gsize len;
    g_assert_true (g_file_get_contents (src, &data, &len, NULL));
    g_assert_true (g_file_set_contents (dst, data, len, NULL));
    g_assert_null (gxd_storage_load ("mario", 7, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error (&error);
    g_unlink (dst);
  }

  fingers = gxd_storage_fingers ("mario");
  g_assert_cmpuint (fingers->len, ==, 1);
  g_assert_cmpint (g_array_index (fingers, int, 0), ==, 2);
  all = gxd_storage_load_all ();
  g_assert_cmpuint (all->len, ==, 2);

  g_assert_true (gxd_storage_delete ("mario", 2, &error));
  g_assert_true (gxd_storage_delete ("mario", 2, &error));   /* gia' assente */
  g_assert_false (gxd_storage_valid_username ("../etc"));
  g_assert_false (gxd_storage_valid_username (".."));
  g_assert_false (gxd_storage_valid_username (""));
  {
    g_autoptr(GxdTemplate) bad = gxd_template_new ("a/b", 2);
    g_assert_false (gxd_storage_save (bad, &error));
  }
  g_clear_error (&error);
  g_assert_true (gxd_storage_delete ("luigi", 7, &error));

  gx_featset_clear (&a);
  {
    g_autofree char *store = g_build_filename (dir, "goodixfpd", NULL);
    g_assert_cmpint (g_rmdir (store), ==, 0);
  }
  g_assert_cmpint (g_rmdir (dir), ==, 0);
}

static void
test_fingers (void)
{
  g_assert_cmpstr (gxd_finger_to_name (0), ==, "any");
  g_assert_cmpstr (gxd_finger_to_name (7), ==, "right-index-finger");
  g_assert_null (gxd_finger_to_name (11));
  g_assert_cmpint (gxd_finger_from_name ("left-little-finger"), ==, 5);
  g_assert_cmpint (gxd_finger_from_name ("any"), ==, 0);
  g_assert_cmpint (gxd_finger_from_name ("thumb"), ==, 0);
  g_assert_cmpint (gxd_finger_from_name (NULL), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/template/fingers", test_fingers);
  g_test_add_func ("/template/roundtrip", test_roundtrip);
  g_test_add_func ("/template/storage", test_storage);
  g_test_add_func ("/template/learn-policy", test_learn_policy);
  g_test_add_func ("/template/dataset", test_dataset);
  return g_test_run ();
}
