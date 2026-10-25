/* gxd-storage.c - impronte su disco.
 *
 * Una impronta per file, <state>/goodixfpd/<utente>/<nome-dito>, directory
 * 0700 e file 0600 (le impronte di libfprint stanno in <state>/<utente>/,
 * qui non si toccano). Scrittura atomica: file temporaneo + rename.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <glib/gstdio.h>

#include "gxd.h"
#include "gxd-storage.h"

#define STORE_SUBDIR "goodixfpd"

gboolean
gxd_storage_valid_username (const char *username)
{
  return username && *username && strlen (username) < 256 &&
         g_utf8_validate (username, -1, NULL) &&
         !strchr (username, '/') &&
         !g_str_equal (username, ".") && !g_str_equal (username, "..");
}

static char *
user_dir (const char *username)
{
  return g_build_filename (gxd_state_dir (), STORE_SUBDIR, username, NULL);
}

static char *
print_path (const char *username, int finger)
{
  return g_build_filename (gxd_state_dir (), STORE_SUBDIR, username,
                           gxd_finger_to_name (finger), NULL);
}

static gboolean
check_args (const char *username, int finger, GError **error)
{
  if (!gxd_storage_valid_username (username) ||
      finger < GXD_FINGER_FIRST || finger > GXD_FINGER_LAST)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "invalid user or finger (%d)", finger);
      return FALSE;
    }
  return TRUE;
}

GxdTemplate *
gxd_storage_load (const char *username, int finger, GError **error)
{
  g_autofree char *path = NULL;
  g_autofree char *contents = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GError) local_error = NULL;
  GxdTemplate *tpl;
  gsize len;

  if (!check_args (username, finger, error))
    return NULL;
  path = print_path (username, finger);
  if (!g_file_get_contents (path, &contents, &len, &local_error))
    {
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "finger %d of %s is not enrolled", finger, username);
      else
        g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }
  bytes = g_bytes_new_take (g_steal_pointer (&contents), len);
  tpl = gxd_template_deserialize (bytes, error);
  if (!tpl)
    {
      g_prefix_error (error, "%s: ", path);
      return NULL;
    }
  /* L'identita' viene dal percorso, non dal contenuto del file. */
  if (!g_str_equal (tpl->username, username) || tpl->finger != finger)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "%s: print belongs to %s/%d", path, tpl->username, tpl->finger);
      gxd_template_unref (tpl);
      return NULL;
    }
  return tpl;
}

gboolean
gxd_storage_save (const GxdTemplate *tpl, GError **error)
{
  g_autofree char *dir = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autofree char *base = NULL;

  if (!check_args (tpl->username, tpl->finger, error))
    return FALSE;
  base = g_build_filename (gxd_state_dir (), STORE_SUBDIR, NULL);
  dir = user_dir (tpl->username);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      int e = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e),
                   "cannot create %s: %s", dir, g_strerror (e));
      return FALSE;
    }
  g_chmod (base, 0700);
  g_chmod (dir, 0700);

  bytes = gxd_template_serialize (tpl);
  if (!bytes)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "cannot serialize print");
      return FALSE;
    }
  path = print_path (tpl->username, tpl->finger);
  return g_file_set_contents_full (path, g_bytes_get_data (bytes, NULL),
                                   g_bytes_get_size (bytes),
                                   G_FILE_SET_CONTENTS_CONSISTENT |
                                   G_FILE_SET_CONTENTS_DURABLE,
                                   0600, error);
}

gboolean
gxd_storage_delete (const char *username, int finger, GError **error)
{
  g_autofree char *path = NULL;
  g_autofree char *dir = NULL;

  if (!check_args (username, finger, error))
    return FALSE;
  path = print_path (username, finger);
  if (g_unlink (path) != 0 && errno != ENOENT)
    {
      int e = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e),
                   "cannot delete %s: %s", path, g_strerror (e));
      return FALSE;
    }
  dir = user_dir (username);
  g_rmdir (dir);                /* solo se ormai vuota */
  return TRUE;
}

GArray *
gxd_storage_fingers (const char *username)
{
  GArray *fingers = g_array_new (FALSE, FALSE, sizeof (int));

  if (!gxd_storage_valid_username (username))
    return fingers;
  for (int f = GXD_FINGER_FIRST; f <= GXD_FINGER_LAST; f++)
    {
      g_autofree char *path = print_path (username, f);

      if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
        g_array_append_val (fingers, f);
    }
  return fingers;
}

GPtrArray *
gxd_storage_load_user (const char *username)
{
  GPtrArray *res = g_ptr_array_new_with_free_func ((GDestroyNotify) gxd_template_unref);
  g_autoptr(GArray) fingers = gxd_storage_fingers (username);

  for (guint i = 0; i < fingers->len; i++)
    {
      g_autoptr(GError) error = NULL;
      GxdTemplate *tpl = gxd_storage_load (username, g_array_index (fingers, int, i), &error);

      if (tpl)
        g_ptr_array_add (res, tpl);
      else
        g_warning ("Skipping unreadable print: %s", error->message);
    }
  return res;
}

GPtrArray *
gxd_storage_load_all (void)
{
  GPtrArray *res = g_ptr_array_new_with_free_func ((GDestroyNotify) gxd_template_unref);
  g_autofree char *base = g_build_filename (gxd_state_dir (), STORE_SUBDIR, NULL);
  g_autoptr(GDir) dir = g_dir_open (base, 0, NULL);
  const char *name;

  if (!dir)
    return res;
  while ((name = g_dir_read_name (dir)) != NULL)
    {
      g_autoptr(GPtrArray) prints = gxd_storage_load_user (name);

      for (guint i = 0; i < prints->len; i++)
        g_ptr_array_add (res, gxd_template_ref (g_ptr_array_index (prints, i)));
    }
  return res;
}
