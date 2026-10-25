/* gxd-template.h - impronta registrata: insieme di frame SIFT di un dito.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <glib.h>

#include "goodix_sift.h"

/* Regole di confronto identiche a quelle di SIGFM in libfprint con il
 * driver goodixgf: c'e' corrispondenza se almeno un frame registrato da'
 * punteggio >= soglia. */
#define GXD_MATCH_THRESHOLD   20
/* Un frame con meno punti chiave non si registra ne' si confronta
 * (in libfprint: "No enough keypoints found" -> riprova). */
#define GXD_MIN_KEYPOINTS     25

typedef enum {
  GXD_FRAME_ENROLLED = 0,     /* acquisito durante la registrazione */
  GXD_FRAME_LEARNED  = 1,     /* aggiunto dall'apprendimento in verifica */
} GxdFrameOrigin;

typedef struct {
  struct gx_featset fs;
  guint8            origin;   /* GxdFrameOrigin */
  guint32           hits;     /* verifiche decise da questo frame */
  gint64            added;    /* tempo unix */
  gint64            last_used;
} GxdFrame;

typedef struct {
  gint       ref;
  char      *username;
  int        finger;
  gint64     enrolled;        /* tempo unix */
  GPtrArray *frames;          /* GxdFrame* */
} GxdTemplate;

typedef struct {
  gboolean matched;
  int      best_score;        /* massimo su tutti i frame */
  int      best_frame;        /* indice del frame migliore, -1 se nessuno */
  int      frames_matched;    /* frame con punteggio >= soglia */
} GxdMatch;

GxdTemplate *gxd_template_new (const char *username, int finger);
GxdTemplate *gxd_template_ref (GxdTemplate *tpl);
void         gxd_template_unref (GxdTemplate *tpl);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GxdTemplate, gxd_template_unref)

/* Aggiunge un frame copiandone le feature. */
void gxd_template_add (GxdTemplate *tpl, const struct gx_featset *fs,
                       GxdFrameOrigin origin, gint64 now);

/* Confronta un'acquisizione con tutti i frame. */
void gxd_template_match (const GxdTemplate *tpl, const struct gx_featset *probe,
                         GxdMatch *out);

/* Apprendimento in verifica (come identifyUpdate/templateStudy del driver
 * Windows): una verifica riuscita con margine e in una zona poco coperta
 * aggiunge l'acquisizione come frame appreso, cosi' la copertura del dito
 * cresce con l'uso. I frame della registrazione non si sostituiscono mai;
 * oltre la capienza esce il frame appreso piu' vecchio.
 * Ritorna TRUE se l'impronta e' cambiata (va salvata). */
#define GXD_LEARN_MIN_SCORE    40   /* 2x la soglia di accettazione */
#define GXD_LEARN_MAX_OVERLAP   3   /* frame che gia' riconoscono l'acquisizione */
#define GXD_LEARN_CAPACITY     30   /* frame appresi al massimo */

gboolean gxd_template_learn (GxdTemplate *tpl, const struct gx_featset *probe,
                             const GxdMatch *m, gint64 now);

/* Formato su disco (versionato). NULL se i dati non sono validi. */
GBytes      *gxd_template_serialize (const GxdTemplate *tpl);
GxdTemplate *gxd_template_deserialize (GBytes *data, GError **error);

void gxd_featset_copy (struct gx_featset *dst, const struct gx_featset *src);
