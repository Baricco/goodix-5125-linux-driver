/* gxd-device.c - the net.reactivated.Fprint.Device object.
 *
 * Reproduces fprintd's contract (src/device.c, 1.94): sessions with
 * Claim/Release, one operation at a time, VerifyStatus/EnrollStatus signals
 * with the same names and the same "done", polkit checks on the same actions,
 * closing the device when the client that claimed it leaves the bus
 * (pam_fprintd does not call Release after a successful login).
 *
 * The device behaves like the libfprint goodixgf driver it replaces (press
 * sensor with identification), hence like fprintd with that driver:
 *   - verification of one finger, or identification among all of the user's
 *     fingers ("any" with more than one finger enrolled);
 *   - enrollment preceded by a duplicate-check touch against the fingerprints
 *     of all users (num-enroll-stages = stages + 1);
 *   - dynamic enrollment of 15 to 30 touches in different positions.
 * In addition, a successful verification can make the fingerprint grow
 * (gxd_template_learn).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <pwd.h>
#include <polkit/polkit.h>

#include "gxd-device.h"
#include "gxd-storage.h"
#include "gxd-template.h"

/* Enrollment: same values as the goodixgf driver. */
#define ENROLL_MIN_STAGES  15
#define ENROLL_MAX_STAGES  30
#define ENROLL_DUP_STOP     4     /* repeated touches in a row that end it */
#define ENROLL_MIN_DIFF   8.0     /* mean difference (0..255) below which
                                   * two touches are "the same position" */

typedef enum {
  ACTION_NONE,
  ACTION_OPEN,
  ACTION_CLOSE,
  ACTION_VERIFY,
  ACTION_IDENTIFY,
  ACTION_ENROLL,
  ACTION_DELETE,
} Action;

typedef enum {
  PERM_NONE        = 0,
  PERM_VERIFY      = 1 << 0,
  PERM_ENROLL      = 1 << 1,
  PERM_SETUSERNAME = 1 << 2,
} Perm;

/* In order of priority, as in fprintd. */
static const struct {
  Perm        perm;
  const char *action;
} polkit_actions[] = {
  { PERM_VERIFY, "net.reactivated.fprint.device.verify" },
  { PERM_ENROLL, "net.reactivated.fprint.device.enroll" },
  { PERM_SETUSERNAME, "net.reactivated.fprint.device.setusername" },
};

typedef enum {
  STATE_CLAIMED,
  STATE_UNCLAIMED,
  STATE_AUTO_CLAIM,
  STATE_ANYTIME,
} ClaimState;

typedef struct {
  gint     ref;
  char    *sender;
  char    *username;
  gboolean busy;                  /* Claim or Release in progress */
  gboolean verify_status_reported;
  gboolean vanished;
} Session;

struct _GxdDevice
{
  FprintDBusDeviceSkeleton parent;

  GxdSensor       *sensor;
  PolkitAuthority *auth;
  GHashTable      *clients;       /* sender -> watch id */

  GMutex           lock;          /* session: also read by the authorization thread */
  Session         *session;

  Action           action;
  GCancellable    *open_cancellable;
  GDBusMethodInvocation *pending;          /* Claim/Release in progress */
  GDBusMethodInvocation *cancel_invocation; /* VerifyStop/EnrollStop waiting */

  /* interruptible operation (verification, identification, enrollment) */
  gboolean         running;
  gboolean         cancelled;
  gboolean         finishing;     /* outcome decided, the scan is stopping */
  gboolean         scan_stopped;
  gboolean         matching;      /* matching in a thread */
  gboolean         done_reported; /* final signal already emitted */
  GQueue           backlog;       /* GxdScans that arrived during a match */

  GPtrArray       *gallery;       /* GxdTemplates to match against */
  gboolean         learn;

  int              enroll_finger;
  gboolean         enroll_dup_phase;
  GPtrArray       *enroll_scans;  /* accepted GxdScans */
  int              enroll_stages;
  int              dup_streak;
  gboolean         converged;
};

static void fprint_device_iface_init (FprintDBusDeviceIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (GxdDevice, gxd_device, FPRINT_DBUS_TYPE_DEVICE_SKELETON,
                               G_IMPLEMENT_INTERFACE (FPRINT_DBUS_TYPE_DEVICE,
                                                      fprint_device_iface_init))

static GQuark quark_auth_user;

static void process_scan (GxdDevice *self, GxdScan *scan);
static void close_session (GxdDevice *self);
static void op_finish (GxdDevice *self, gboolean cancelled);

/* ---- session ---- */

static Session *
session_ref (Session *s)
{
  g_atomic_int_inc (&s->ref);
  return s;
}

static void
session_unref (Session *s)
{
  if (s && g_atomic_int_dec_and_test (&s->ref))
    {
      g_free (s->sender);
      g_free (s->username);
      g_free (s);
    }
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Session, session_unref)

static Session *
session_get (GxdDevice *self)
{
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&self->lock);

  return self->session ? session_ref (self->session) : NULL;
}

static void
session_set (GxdDevice *self, Session *s)
{
  Session *old;

  g_mutex_lock (&self->lock);
  old = self->session;
  self->session = s;
  g_mutex_unlock (&self->lock);
  session_unref (old);
}

static void
session_set_busy (GxdDevice *self, gboolean busy)
{
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&self->lock);

  if (self->session)
    self->session->busy = busy;
}

/* ---- client on the bus ---- */

static void
client_vanished (GDBusConnection *connection, const char *name, gpointer data)
{
  GxdDevice *self = data;
  g_autoptr(Session) session = session_get (self);

  if (session && g_str_equal (session->sender, name))
    {
      g_debug ("Client %s vanished, releasing the device", name);
      session->vanished = TRUE;
      switch (self->action)
        {
        case ACTION_OPEN:
          g_cancellable_cancel (self->open_cancellable);  /* close when opening completes */
          break;
        case ACTION_CLOSE:
          break;                                          /* already closing */
        default:
          if (self->running)
            op_finish (self, TRUE);     /* close once the operation is over */
          else
            close_session (self);
        }
    }
  g_hash_table_remove (self->clients, name);
}

static void
unwatch (gpointer id)
{
  g_bus_unwatch_name (GPOINTER_TO_UINT (id));
}

static void
add_client (GxdDevice *self, GDBusMethodInvocation *invocation)
{
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  GDBusConnection *conn = g_dbus_method_invocation_get_connection (invocation);

  if (g_hash_table_contains (self->clients, sender))
    return;
  g_hash_table_insert (self->clients, g_strdup (sender),
                       GUINT_TO_POINTER (g_bus_watch_name_on_connection (
                                           conn, sender, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                           NULL, client_vanished, self, NULL)));
}

/* ---- authorization (GDBus's dedicated thread) ---- */

static ClaimState
claim_state_for (const char *method)
{
  if (g_str_equal (method, "Claim"))
    return STATE_UNCLAIMED;
  if (g_str_equal (method, "DeleteEnrolledFingers"))
    return STATE_AUTO_CLAIM;
  if (g_str_equal (method, "ListEnrolledFingers"))
    return STATE_ANYTIME;
  return STATE_CLAIMED;
}

static gboolean
check_claimed (GxdDevice *self, GDBusMethodInvocation *invocation, GError **error)
{
  ClaimState state = claim_state_for (g_dbus_method_invocation_get_method_name (invocation));
  g_autoptr(Session) session = NULL;

  if (state == STATE_ANYTIME)
    return TRUE;
  session = session_get (self);
  if (state == STATE_AUTO_CLAIM)
    state = session ? STATE_CLAIMED : STATE_UNCLAIMED;

  if (state == STATE_UNCLAIMED)
    {
      if (!session)
        return TRUE;
      g_set_error_literal (error, GXD_ERROR, GXD_ERROR_ALREADY_IN_USE,
                           "Device was already claimed");
      return FALSE;
    }
  if (!session)
    {
      g_set_error_literal (error, GXD_ERROR, GXD_ERROR_CLAIM_DEVICE,
                           "Device was not claimed before use");
      return FALSE;
    }
  if (!g_str_equal (g_dbus_method_invocation_get_sender (invocation), session->sender) ||
      session->busy)
    {
      g_set_error_literal (error, GXD_ERROR, GXD_ERROR_ALREADY_IN_USE,
                           "Device already in use by another user");
      return FALSE;
    }
  return TRUE;
}

static Perm
perms_for (const char *method)
{
  if (g_str_equal (method, "Claim"))
    return PERM_VERIFY | PERM_ENROLL;
  if (g_str_equal (method, "DeleteEnrolledFinger") ||
      g_str_equal (method, "DeleteEnrolledFingers") ||
      g_str_equal (method, "DeleteEnrolledFingers2") ||
      g_str_equal (method, "EnrollStart"))
    return PERM_ENROLL;
  if (g_str_equal (method, "ListEnrolledFingers") ||
      g_str_equal (method, "VerifyStart"))
    return PERM_VERIFY;
  /* Release, VerifyStop, EnrollStop: whoever could start may stop */
  return PERM_NONE;
}

static gboolean
check_polkit (GxdDevice *self, GDBusMethodInvocation *invocation, Perm perms, GError **error)
{
  g_autoptr(PolkitSubject) subject = NULL;

  if (perms == PERM_NONE)
    return TRUE;
  subject = polkit_system_bus_name_new (g_dbus_method_invocation_get_sender (invocation));
  for (guint i = 0; i < G_N_ELEMENTS (polkit_actions); i++)
    {
      g_autoptr(PolkitAuthorizationResult) result = NULL;
      g_autoptr(GError) local = NULL;

      if (!(perms & polkit_actions[i].perm))
        continue;
      g_clear_error (error);
      result = polkit_authority_check_authorization_sync (
        self->auth, subject, polkit_actions[i].action, NULL,
        POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION, NULL, &local);
      if (!result)
        {
          g_set_error (error, GXD_ERROR, GXD_ERROR_PERMISSION_DENIED,
                       "Not Authorized: %s", local->message);
          continue;
        }
      if (polkit_authorization_result_get_is_authorized (result))
        return TRUE;
      g_set_error (error, GXD_ERROR, GXD_ERROR_PERMISSION_DENIED,
                   "Not Authorized: %s", polkit_actions[i].action);
    }
  return FALSE;
}

/* User being operated on: the caller, or the requested one if the caller
 * holds net.reactivated.fprint.device.setusername. */
static char *
resolve_username (GxdDevice *self, GDBusMethodInvocation *invocation,
                  const char *requested, GError **error)
{
  GDBusConnection *conn = g_dbus_method_invocation_get_connection (invocation);
  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GError) local = NULL;
  struct passwd pw, *res = NULL;
  char buf[4096];
  guint32 uid;

  ret = g_dbus_connection_call_sync (conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                     "org.freedesktop.DBus", "GetConnectionUnixUser",
                                     g_variant_new ("(s)", g_dbus_method_invocation_get_sender (invocation)),
                                     G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, -1,
                                     NULL, &local);
  if (!ret)
    {
      g_set_error (error, GXD_ERROR, GXD_ERROR_INTERNAL,
                   "Could not get connection unix user ID: %s", local->message);
      return NULL;
    }
  g_variant_get (ret, "(u)", &uid);
  if (getpwuid_r (uid, &pw, buf, sizeof (buf), &res) != 0 || res == NULL)
    {
      g_set_error (error, GXD_ERROR, GXD_ERROR_INTERNAL,
                   "Failed to get information about user UID %u", uid);
      return NULL;
    }
  if (requested == NULL || *requested == '\0' || g_str_equal (requested, pw.pw_name))
    return g_strdup (pw.pw_name);
  if (!check_polkit (self, invocation, PERM_SETUSERNAME, error))
    return NULL;
  return g_strdup (requested);
}

static gboolean
on_authorize (GDBusInterfaceSkeleton *iface, GDBusMethodInvocation *invocation, gpointer data)
{
  GxdDevice *self = GXD_DEVICE (iface);
  const char *method = g_dbus_method_invocation_get_method_name (invocation);
  g_autoptr(GError) error = NULL;

  if (!check_claimed (self, invocation, &error))
    goto denied;
  if (g_str_equal (method, "Claim") || g_str_equal (method, "DeleteEnrolledFingers") ||
      g_str_equal (method, "ListEnrolledFingers"))
    {
      const char *requested = NULL;
      char *user;

      g_variant_get (g_dbus_method_invocation_get_parameters (invocation), "(&s)", &requested);
      user = resolve_username (self, invocation, requested, &error);
      if (!user)
        goto denied;
      if (!gxd_storage_valid_username (user))
        {
          g_free (user);
          g_set_error_literal (&error, GXD_ERROR, GXD_ERROR_INTERNAL, "Invalid user name");
          goto denied;
        }
      g_object_set_qdata_full (G_OBJECT (invocation), quark_auth_user, user, g_free);
    }
  if (!check_polkit (self, invocation, perms_for (method), &error))
    goto denied;
  return TRUE;

denied:
  g_warning ("Authorization denied to %s to call method '%s': %s",
             g_dbus_method_invocation_get_sender (invocation), method, error->message);
  g_dbus_method_invocation_return_gerror (invocation, error);
  return FALSE;
}

/* ---- operation state ---- */

static gboolean
can_start_action (GxdDevice *self, GError **error)
{
  const char *msg;

  switch (self->action)
    {
    case ACTION_NONE:
      return TRUE;
    case ACTION_ENROLL:
      msg = "Enrollment already in progress";
      break;
    case ACTION_VERIFY:
    case ACTION_IDENTIFY:
      msg = "Verification already in progress";
      break;
    case ACTION_OPEN:
      msg = "Claim already in progress";
      break;
    case ACTION_CLOSE:
      msg = "Release already in progress";
      break;
    case ACTION_DELETE:
      msg = "Delete already in progress";
      break;
    default:
      msg = "Another operation is already in progress";
    }
  g_set_error_literal (error, GXD_ERROR, GXD_ERROR_ALREADY_IN_USE, msg);
  return FALSE;
}

static gboolean
can_stop_action (GxdDevice *self, Action action, GError **error)
{
  gboolean matches;

  if (self->action == ACTION_VERIFY || self->action == ACTION_IDENTIFY)
    matches = action == ACTION_VERIFY || action == ACTION_IDENTIFY;
  else
    matches = self->action == action;

  if (matches && !self->cancel_invocation)
    return TRUE;
  if (self->action != ACTION_NONE || matches)
    {
      g_set_error_literal (error, GXD_ERROR, GXD_ERROR_ALREADY_IN_USE,
                           "Another operation is already in progress");
      return FALSE;
    }
  g_set_error_literal (error, GXD_ERROR, GXD_ERROR_NO_ACTION_IN_PROGRESS,
                       action == ACTION_ENROLL ? "No enrollment in progress" :
                       action == ACTION_VERIFY ? "No verification in progress" :
                       "No action in progress");
  return FALSE;
}

static void
report_verify (GxdDevice *self, const char *result, gboolean done)
{
  g_autoptr(Session) session = session_get (self);

  if (!session)
    return;
  if (done && session->verify_status_reported)
    return;
  g_debug ("VerifyStatus %s (done %d)", result, done);
  fprint_dbus_device_emit_verify_status (FPRINT_DBUS_DEVICE (self), result, done);
  if (done)
    {
      session->verify_status_reported = TRUE;
      self->done_reported = TRUE;
    }
}

static void
report_enroll (GxdDevice *self, const char *result, gboolean done)
{
  if (self->done_reported)
    return;
  g_debug ("EnrollStatus %s (done %d)", result, done);
  fprint_dbus_device_emit_enroll_status (FPRINT_DBUS_DEVICE (self), result, done);
  if (done)
    self->done_reported = TRUE;
}

static void
set_enroll_stages (GxdDevice *self, int stages)
{
  self->enroll_stages = stages;
  /* +1: the duplicate-check touch, as fprintd does with devices that can
   * identify */
  fprint_dbus_device_set_num_enroll_stages (FPRINT_DBUS_DEVICE (self), stages + 1);
}

static void
op_reset (GxdDevice *self)
{
  g_clear_pointer (&self->gallery, g_ptr_array_unref);
  g_clear_pointer (&self->enroll_scans, g_ptr_array_unref);
  g_queue_clear_full (&self->backlog, (GDestroyNotify) gxd_scan_free);
  self->running = FALSE;
  self->cancelled = FALSE;
  self->finishing = FALSE;
  self->done_reported = FALSE;
  fprint_dbus_device_set_finger_needed (FPRINT_DBUS_DEVICE (self), FALSE);
  fprint_dbus_device_set_finger_present (FPRINT_DBUS_DEVICE (self), FALSE);
}

/* The operation is over (scan stopped, no match in flight). */
static void
maybe_completed (GxdDevice *self)
{
  g_autoptr(Session) session = NULL;
  Action action = self->action;

  if (!self->running || !self->finishing || !self->scan_stopped || self->matching)
    return;

  if (self->cancelled)
    {
      /* Like libfprint/fprintd on G_IO_ERROR_CANCELLED */
      if (action == ACTION_ENROLL)
        report_enroll (self, "enroll-failed", TRUE);
      else
        report_verify (self, "verify-no-match", TRUE);
    }
  op_reset (self);

  session = session_get (self);
  if (self->cancel_invocation)
    {
      /* complete_* consumes the invocation's reference, the autoptr ours */
      g_autoptr(GDBusMethodInvocation) stop = g_steal_pointer (&self->cancel_invocation);

      if (action == ACTION_ENROLL)
        fprint_dbus_device_complete_enroll_stop (FPRINT_DBUS_DEVICE (self), stop);
      else
        fprint_dbus_device_complete_verify_stop (FPRINT_DBUS_DEVICE (self), stop);
      self->action = ACTION_NONE;
      if (session)
        session->verify_status_reported = FALSE;
    }
  else if (self->pending || (session && session->vanished))
    {
      /* Release waiting or client gone: close */
      self->action = ACTION_NONE;
      if (session)
        session->verify_status_reported = FALSE;
    }

  if (session && session->vanished)
    close_session (self);
  else if (self->pending)
    close_session (self);
}

static void
scan_stopped_cb (GObject *source, GAsyncResult *res, gpointer data)
{
  GxdDevice *self = data;

  gxd_sensor_scan_stop_finish (GXD_SENSOR (source), res);
  self->scan_stopped = TRUE;
  maybe_completed (self);
  g_object_unref (self);
}

/* Outcome decided (or cancellation): stop the scan. */
static void
op_finish (GxdDevice *self, gboolean cancelled)
{
  if (!self->running)
    return;
  if (cancelled)
    self->cancelled = TRUE;
  if (self->finishing)
    {
      maybe_completed (self);
      return;
    }
  self->finishing = TRUE;
  self->scan_stopped = FALSE;
  gxd_sensor_scan_stop (self->sensor, scan_stopped_cb, g_object_ref (self));
}

static void
op_start (GxdDevice *self, Action action)
{
  self->action = action;
  self->running = TRUE;
  self->cancelled = FALSE;
  self->finishing = FALSE;
  self->done_reported = FALSE;
  self->scan_stopped = FALSE;
  fprint_dbus_device_set_finger_needed (FPRINT_DBUS_DEVICE (self), TRUE);
  gxd_sensor_scan_start (self->sensor);
}

/* ---- matching in a thread ---- */

typedef struct {
  GPtrArray        *gallery;      /* GxdTemplate */
  struct gx_featset probe;
  gboolean          learn;
  int               matched;      /* index in gallery, -1 none */
  GxdMatch          m;
} MatchJob;

static void
match_job_free (MatchJob *job)
{
  g_ptr_array_unref (job->gallery);
  gx_featset_clear (&job->probe);
  g_free (job);
}

static void
match_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  MatchJob *job = data;

  job->matched = -1;
  for (guint i = 0; i < job->gallery->len; i++)
    {
      GxdTemplate *tpl = g_ptr_array_index (job->gallery, i);

      gxd_template_match (tpl, &job->probe, &job->m);
      g_debug ("Print %s/%s: best score %d, %d frames over threshold",
               tpl->username, gxd_finger_to_name (tpl->finger),
               job->m.best_score, job->m.frames_matched);
      if (job->m.matched)
        {
          job->matched = (int) i;
          break;
        }
    }

  if (job->matched >= 0 && job->learn)
    {
      GxdTemplate *tpl = g_ptr_array_index (job->gallery, job->matched);
      g_autoptr(GError) error = NULL;

      if (gxd_template_learn (tpl, &job->probe, &job->m, g_get_real_time () / G_USEC_PER_SEC))
        {
          if (gxd_storage_save (tpl, &error))
            g_message ("Learned a new area for %s/%s (%u frames)", tpl->username,
                       gxd_finger_to_name (tpl->finger), tpl->frames->len);
          else
            g_warning ("Could not save learned print: %s", error->message);
        }
    }
  g_task_return_boolean (task, TRUE);
}

static void match_done (GObject *source, GAsyncResult *res, gpointer data);

static void
start_match (GxdDevice *self, GPtrArray *gallery, const struct gx_featset *probe, gboolean learn)
{
  MatchJob *job = g_new0 (MatchJob, 1);
  g_autoptr(GTask) task = g_task_new (self, NULL, match_done, NULL);

  job->gallery = g_ptr_array_ref (gallery);
  gxd_featset_copy (&job->probe, probe);
  job->learn = learn;
  self->matching = TRUE;
  g_task_set_task_data (task, job, (GDestroyNotify) match_job_free);
  g_task_run_in_thread (task, match_thread);
}

static void
match_done (GObject *source, GAsyncResult *res, gpointer data)
{
  GxdDevice *self = GXD_DEVICE (source);
  MatchJob *job = g_task_get_task_data (G_TASK (res));
  gboolean matched = job->matched >= 0;

  self->matching = FALSE;
  if (!self->running || self->finishing)
    {
      maybe_completed (self);
      return;
    }

  switch (self->action)
    {
    case ACTION_VERIFY:
    case ACTION_IDENTIFY:
      report_verify (self, matched ? "verify-match" : "verify-no-match", TRUE);
      op_finish (self, FALSE);
      return;

    case ACTION_ENROLL:
      if (matched)
        {
          GxdTemplate *dup = g_ptr_array_index (job->gallery, job->matched);
          g_message ("Enrollment refused: finger already enrolled as %s/%s",
                     dup->username, gxd_finger_to_name (dup->finger));
          report_enroll (self, "enroll-duplicate", TRUE);
          op_finish (self, FALSE);
          return;
        }
      self->enroll_dup_phase = FALSE;
      fprint_dbus_device_emit_enroll_status (FPRINT_DBUS_DEVICE (self),
                                             "enroll-stage-passed", FALSE);
      break;

    default:
      g_assert_not_reached ();
    }

  /* touches that arrived during the match */
  while (self->running && !self->finishing && !self->matching &&
         !g_queue_is_empty (&self->backlog))
    {
      GxdScan *scan = g_queue_pop_head (&self->backlog);
      process_scan (self, scan);
      gxd_scan_free (scan);
    }
}

/* ---- enrollment ---- */

static gboolean
enroll_same_place (GxdDevice *self, const GxdScan *scan)
{
  const char *e = g_getenv ("GOODIX_ENROLL_MIN_DIFF");
  double min_diff = e && *e ? g_ascii_strtod (e, NULL) : ENROLL_MIN_DIFF;
  const int npix = GXD_IMG_W * GXD_IMG_H;

  if (min_diff <= 0.0)
    return FALSE;
  for (guint i = 0; i < self->enroll_scans->len; i++)
    {
      const GxdScan *prev = g_ptr_array_index (self->enroll_scans, i);
      gint64 sum = 0;

      for (int k = 0; k < npix; k++)
        sum += ABS ((int) scan->img8[k] - (int) prev->img8[k]);
      if ((double) sum / npix < min_diff)
        return TRUE;
    }
  return FALSE;
}

static void
enroll_complete (GxdDevice *self)
{
  g_autoptr(Session) session = session_get (self);
  g_autoptr(GxdTemplate) tpl = NULL;
  g_autoptr(GError) error = NULL;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;

  if (!session)
    {
      report_enroll (self, "enroll-failed", TRUE);
      op_finish (self, FALSE);
      return;
    }
  tpl = gxd_template_new (session->username, self->enroll_finger);
  tpl->enrolled = now;
  for (guint i = 0; i < self->enroll_scans->len; i++)
    {
      GxdScan *s = g_ptr_array_index (self->enroll_scans, i);
      gxd_template_add (tpl, &s->fs, GXD_FRAME_ENROLLED, now);
    }
  if (gxd_storage_save (tpl, &error))
    {
      g_message ("Enrolled %s/%s with %u frames", tpl->username,
                 gxd_finger_to_name (tpl->finger), tpl->frames->len);
      report_enroll (self, "enroll-completed", TRUE);
    }
  else
    {
      g_warning ("Could not save the new print: %s", error->message);
      report_enroll (self, "enroll-failed", TRUE);
    }
  op_finish (self, FALSE);
}

static void
enroll_frame (GxdDevice *self, GxdScan *scan)
{
  int accepted = self->enroll_scans->len;

  /* Different positions: a touch equal to one already taken adds no
   * coverage. If the user runs out of new positions (DUP_STOP repeated
   * touches in a row, after at least MIN_STAGES) enrollment ends with this
   * touch, lowering the number of stages. */
  if (!self->converged && accepted > 0 && enroll_same_place (self, scan))
    {
      self->dup_streak++;
      if (accepted >= ENROLL_MIN_STAGES && accepted < ENROLL_MAX_STAGES &&
          self->dup_streak >= ENROLL_DUP_STOP)
        {
          self->converged = TRUE;
          g_message ("Enrollment coverage converged at %d stages", accepted + 1);
          set_enroll_stages (self, accepted + 1);
        }
      else
        {
          g_message ("Enrollment press too similar to a previous one");
          report_enroll (self, "enroll-retry-scan", FALSE);
          return;
        }
    }
  else
    {
      self->dup_streak = 0;
    }

  {
    GxdScan *keep = g_new0 (GxdScan, 1);
    keep->kind = scan->kind;
    memcpy (keep->img8, scan->img8, sizeof (keep->img8));
    gxd_featset_copy (&keep->fs, &scan->fs);
    g_ptr_array_add (self->enroll_scans, keep);
  }

  if ((int) self->enroll_scans->len >= self->enroll_stages)
    enroll_complete (self);
  else
    report_enroll (self, "enroll-stage-passed", FALSE);
}

/* ---- sensor events ---- */

static void
process_scan (GxdDevice *self, GxdScan *scan)
{
  if (self->matching)
    {
      GxdScan *copy = g_new0 (GxdScan, 1);
      copy->kind = scan->kind;
      memcpy (copy->img8, scan->img8, sizeof (copy->img8));
      gxd_featset_copy (&copy->fs, &scan->fs);
      g_queue_push_tail (&self->backlog, copy);
      return;
    }

  switch (self->action)
    {
    case ACTION_VERIFY:
    case ACTION_IDENTIFY:
      if (scan->kind == GXD_SCAN_RETRY)
        report_verify (self, "verify-retry-scan", FALSE);
      else
        start_match (self, self->gallery, &scan->fs, self->learn);
      break;

    case ACTION_ENROLL:
      if (scan->kind == GXD_SCAN_RETRY)
        report_enroll (self, "enroll-retry-scan", FALSE);
      else if (self->enroll_dup_phase)
        start_match (self, self->gallery, &scan->fs, FALSE);
      else
        enroll_frame (self, scan);
      break;

    default:
      break;
    }
}

static void
on_scan (GxdSensor *sensor, GxdScan *scan, gpointer data)
{
  GxdDevice *self = data;

  if (!self->running || self->finishing)
    return;
  process_scan (self, scan);
}

static void
on_finger (GxdSensor *sensor, gboolean present, gpointer data)
{
  GxdDevice *self = data;

  if (!self->running)
    return;
  fprint_dbus_device_set_finger_present (FPRINT_DBUS_DEVICE (self), present);
  fprint_dbus_device_set_finger_needed (FPRINT_DBUS_DEVICE (self), !present);
}

static void
on_lost (GxdSensor *sensor, GError *error, gpointer data)
{
  GxdDevice *self = data;

  if (!self->running || self->finishing)
    return;
  g_warning ("Sensor lost during the operation: %s", error->message);
  if (self->action == ACTION_ENROLL)
    report_enroll (self, "enroll-disconnected", TRUE);
  else
    report_verify (self, "verify-disconnected", TRUE);
  op_finish (self, FALSE);
}

/* ---- Claim / Release ---- */

static void
close_done (GObject *source, GAsyncResult *res, gpointer data)
{
  GxdDevice *self = data;
  /* pending holds an extra reference: the autoptr releases it, the reply
   * consumes the invocation's own */
  g_autoptr(GDBusMethodInvocation) invocation = g_steal_pointer (&self->pending);

  gxd_sensor_close_finish (GXD_SENSOR (source), res);
  session_set (self, NULL);
  self->action = ACTION_NONE;
  g_debug ("Device released");
  if (invocation)
    fprint_dbus_device_complete_release (FPRINT_DBUS_DEVICE (self), invocation);
  g_object_unref (self);
}

/* Closes the device and ends the session (Release or client gone).
 * To be called with no operation running. */
static void
close_session (GxdDevice *self)
{
  self->action = ACTION_CLOSE;
  session_set_busy (self, TRUE);
  gxd_sensor_close (self->sensor, close_done, g_object_ref (self));
}

static void
open_done (GObject *source, GAsyncResult *res, gpointer data)
{
  GxdDevice *self = data;
  g_autoptr(GDBusMethodInvocation) invocation = g_steal_pointer (&self->pending);
  g_autoptr(Session) session = session_get (self);
  g_autoptr(GError) error = NULL;
  gboolean ok = gxd_sensor_open_finish (GXD_SENSOR (source), res, &error);

  g_clear_object (&self->open_cancellable);
  self->action = ACTION_NONE;
  session_set_busy (self, FALSE);

  if (session && session->vanished)
    {
      /* the client left while the device was opening */
      g_dbus_method_invocation_return_error_literal (invocation, GXD_ERROR,
                                                     GXD_ERROR_INTERNAL,
                                                     "Client disconnected during claim");
      if (ok)
        close_session (self);
      else
        session_set (self, NULL);
    }
  else if (!ok)
    {
      g_warning ("Could not open the sensor: %s", error->message);
      session_set (self, NULL);
      g_dbus_method_invocation_return_error (invocation, GXD_ERROR,
                                             GXD_ERROR_INTERNAL, "Open failed with error: %s",
                                             error->message);
    }
  else
    {
      g_debug ("Device claimed by %s for %s", session->sender, session->username);
      fprint_dbus_device_complete_claim (FPRINT_DBUS_DEVICE (self), invocation);
    }
  g_object_unref (self);
}

static gboolean
handle_claim (FprintDBusDevice *dbus, GDBusMethodInvocation *invocation, const char *username)
{
  GxdDevice *self = GXD_DEVICE (dbus);
  g_autoptr(GError) error = NULL;
  Session *s;

  if (!check_claimed (self, invocation, &error) || !can_start_action (self, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  add_client (self, invocation);

  s = g_new0 (Session, 1);
  s->ref = 1;
  s->sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  s->username = g_strdup (g_object_get_qdata (G_OBJECT (invocation), quark_auth_user));
  s->busy = TRUE;
  session_set (self, s);

  self->action = ACTION_OPEN;
  self->pending = g_object_ref (invocation);
  self->open_cancellable = g_cancellable_new ();
  gxd_sensor_open (self->sensor, self->open_cancellable, open_done, g_object_ref (self));
  return TRUE;
}

static gboolean
handle_release (FprintDBusDevice *dbus, GDBusMethodInvocation *invocation)
{
  GxdDevice *self = GXD_DEVICE (dbus);
  g_autoptr(GError) error = NULL;

  if (!check_claimed (self, invocation, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  session_set_busy (self, TRUE);
  self->pending = g_object_ref (invocation);
  if (self->running)
    {
      g_warning ("%s was in progress, stopping it",
                 self->action == ACTION_ENROLL ? "Enrollment" : "Verification");
      op_finish (self, TRUE);           /* close once the operation has stopped */
      return TRUE;
    }
  close_session (self);
  return TRUE;
}

/* ---- verification ---- */

static gboolean
handle_verify_start (FprintDBusDevice *dbus, GDBusMethodInvocation *invocation,
                     const char *finger_name)
{
  GxdDevice *self = GXD_DEVICE (dbus);
  g_autoptr(GError) error = NULL;
  g_autoptr(Session) session = NULL;
  g_autoptr(GPtrArray) gallery = NULL;
  int finger = gxd_finger_from_name (finger_name);
  Action action = ACTION_VERIFY;

  if (!check_claimed (self, invocation, &error) || !can_start_action (self, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  session = session_get (self);

  if (finger == GXD_FINGER_UNKNOWN)
    {
      gallery = gxd_storage_load_user (session->username);
      if (gallery->len == 0)
        {
          g_dbus_method_invocation_return_error_literal (invocation, GXD_ERROR,
                                                         GXD_ERROR_NO_ENROLLED_PRINTS,
                                                         "No fingerprints enrolled");
          return TRUE;
        }
      if (gallery->len == 1)
        finger = ((GxdTemplate *) g_ptr_array_index (gallery, 0))->finger;
      else
        action = ACTION_IDENTIFY;
    }
  else
    {
      GxdTemplate *tpl = gxd_storage_load (session->username, finger, NULL);

      if (!tpl)
        {
          g_dbus_method_invocation_return_error (invocation, GXD_ERROR,
                                                 GXD_ERROR_NO_ENROLLED_PRINTS,
                                                 "No such print %d", finger);
          return TRUE;
        }
      gallery = g_ptr_array_new_with_free_func ((GDestroyNotify) gxd_template_unref);
      g_ptr_array_add (gallery, tpl);
    }

  g_debug ("Start %s for %s, finger %s",
           action == ACTION_IDENTIFY ? "identification" : "verification",
           session->username, gxd_finger_to_name (finger));
  session->verify_status_reported = FALSE;
  self->gallery = g_steal_pointer (&gallery);
  op_start (self, action);

  fprint_dbus_device_complete_verify_start (dbus, invocation);
  fprint_dbus_device_emit_verify_finger_selected (dbus, gxd_finger_to_name (finger));
  return TRUE;
}

static gboolean
handle_verify_stop (FprintDBusDevice *dbus, GDBusMethodInvocation *invocation)
{
  GxdDevice *self = GXD_DEVICE (dbus);
  g_autoptr(GError) error = NULL;
  g_autoptr(Session) session = NULL;

  if (!check_claimed (self, invocation, &error) ||
      !can_stop_action (self, ACTION_VERIFY, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  if (self->running)
    {
      /* With an outcome already given, wait for the natural end (a few ms),
       * otherwise cancel. The reply is sent once the operation has stopped. */
      self->cancel_invocation = g_object_ref (invocation);
      op_finish (self, !self->finishing);
      return TRUE;
    }
  self->action = ACTION_NONE;
  session = session_get (self);
  if (session)
    session->verify_status_reported = FALSE;
  fprint_dbus_device_complete_verify_stop (dbus, invocation);
  return TRUE;
}

/* ---- enrollment ---- */

static gboolean
user_has_finger (const char *username, int finger)
{
  g_autoptr(GArray) fingers = gxd_storage_fingers (username);

  if (finger == GXD_FINGER_UNKNOWN)
    return fingers->len > 0;
  for (guint i = 0; i < fingers->len; i++)
    if (g_array_index (fingers, int, i) == finger)
      return TRUE;
  return FALSE;
}

static gboolean
delete_fingers (const char *username, int finger, GError **error)
{
  g_autoptr(GError) first = NULL;

  if (!user_has_finger (username, finger))
    {
      if (finger != GXD_FINGER_UNKNOWN)
        g_set_error (error, GXD_ERROR, GXD_ERROR_NO_ENROLLED_PRINTS,
                     "Fingerprint for finger %s is not enrolled", gxd_finger_to_name (finger));
      else
        g_set_error_literal (error, GXD_ERROR, GXD_ERROR_NO_ENROLLED_PRINTS,
                             "No fingerprint enrolled");
      return FALSE;
    }
  for (int f = GXD_FINGER_FIRST; f <= GXD_FINGER_LAST; f++)
    {
      g_autoptr(GError) local = NULL;

      if (finger != GXD_FINGER_UNKNOWN && f != finger)
        continue;
      if (!gxd_storage_delete (username, f, &local) && !first)
        first = g_steal_pointer (&local);
    }
  if (first)
    {
      g_set_error (error, GXD_ERROR, GXD_ERROR_PRINTS_NOT_DELETED,
                   "Impossible to delete fingerprint reference got error: %s", first->message);
      return FALSE;
    }
  g_message ("Deleted %s of %s", finger ? gxd_finger_to_name (finger) : "all fingers", username);
  return TRUE;
}

static gboolean
handle_enroll_start (FprintDBusDevice *dbus, GDBusMethodInvocation *invocation,
                     const char *finger_name)
{
  GxdDevice *self = GXD_DEVICE (dbus);
  g_autoptr(GError) error = NULL;
  g_autoptr(Session) session = NULL;
  int finger = gxd_finger_from_name (finger_name);

  if (!check_claimed (self, invocation, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  if (finger == GXD_FINGER_UNKNOWN)
    {
      g_dbus_method_invocation_return_error_literal (invocation, GXD_ERROR,
                                                     GXD_ERROR_INVALID_FINGERNAME,
                                                     "Invalid finger name");
      return TRUE;
    }
  if (!can_start_action (self, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  session = session_get (self);
  /* Enrolling a finger again replaces the previous fingerprint. */
  if (user_has_finger (session->username, finger) &&
      !delete_fingers (session->username, finger, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  g_debug ("Start enrollment for %s, finger %s", session->username, finger_name);
  self->enroll_finger = finger;
  self->enroll_dup_phase = TRUE;
  self->enroll_scans = g_ptr_array_new_with_free_func ((GDestroyNotify) gxd_scan_free);
  self->dup_streak = 0;
  self->converged = FALSE;
  set_enroll_stages (self, ENROLL_MAX_STAGES);
  /* No finger may be enrolled twice, not even by different users. */
  self->gallery = gxd_storage_load_all ();
  op_start (self, ACTION_ENROLL);

  fprint_dbus_device_complete_enroll_start (dbus, invocation);
  return TRUE;
}

static gboolean
handle_enroll_stop (FprintDBusDevice *dbus, GDBusMethodInvocation *invocation)
{
  GxdDevice *self = GXD_DEVICE (dbus);
  g_autoptr(GError) error = NULL;

  if (!check_claimed (self, invocation, &error) ||
      !can_stop_action (self, ACTION_ENROLL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  if (self->running)
    {
      self->cancel_invocation = g_object_ref (invocation);
      op_finish (self, !self->finishing);
      return TRUE;
    }
  self->action = ACTION_NONE;
  fprint_dbus_device_complete_enroll_stop (dbus, invocation);
  return TRUE;
}

/* ---- listing and deletion ---- */

static gboolean
handle_list_enrolled_fingers (FprintDBusDevice *dbus, GDBusMethodInvocation *invocation,
                              const char *username)
{
  GxdDevice *self = GXD_DEVICE (dbus);
  const char *user = g_object_get_qdata (G_OBJECT (invocation), quark_auth_user);
  g_autoptr(GArray) fingers = NULL;
  g_autoptr(GPtrArray) names = g_ptr_array_new ();

  add_client (self, invocation);
  fingers = gxd_storage_fingers (user);
  for (guint i = 0; i < fingers->len; i++)
    g_ptr_array_add (names, (char *) gxd_finger_to_name (g_array_index (fingers, int, i)));
  if (names->len == 0)
    {
      g_dbus_method_invocation_return_error_literal (invocation, GXD_ERROR,
                                                     GXD_ERROR_NO_ENROLLED_PRINTS,
                                                     "Failed to discover prints");
      return TRUE;
    }
  g_ptr_array_add (names, NULL);
  fprint_dbus_device_complete_list_enrolled_fingers (dbus, invocation,
                                                     (const char *const *) names->pdata);
  return TRUE;
}

static gboolean
handle_delete_enrolled_fingers (FprintDBusDevice *dbus, GDBusMethodInvocation *invocation,
                                const char *username)
{
  GxdDevice *self = GXD_DEVICE (dbus);
  const char *user = g_object_get_qdata (G_OBJECT (invocation), quark_auth_user);
  g_autoptr(GError) error = NULL;

  g_warning ("The API user should be updated to use DeleteEnrolledFingers2 method!");
  if (!can_start_action (self, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  if (!check_claimed (self, invocation, &error) &&
      !g_error_matches (error, GXD_ERROR, GXD_ERROR_CLAIM_DEVICE))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  g_clear_error (&error);
  add_client (self, invocation);
  if (!delete_fingers (user, GXD_FINGER_UNKNOWN, &error))
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    fprint_dbus_device_complete_delete_enrolled_fingers (dbus, invocation);
  return TRUE;
}

static gboolean
handle_delete_enrolled_fingers2 (FprintDBusDevice *dbus, GDBusMethodInvocation *invocation)
{
  GxdDevice *self = GXD_DEVICE (dbus);
  g_autoptr(GError) error = NULL;
  g_autoptr(Session) session = NULL;

  if (!check_claimed (self, invocation, &error) || !can_start_action (self, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  session = session_get (self);
  if (!delete_fingers (session->username, GXD_FINGER_UNKNOWN, &error))
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    fprint_dbus_device_complete_delete_enrolled_fingers2 (dbus, invocation);
  return TRUE;
}

static gboolean
handle_delete_enrolled_finger (FprintDBusDevice *dbus, GDBusMethodInvocation *invocation,
                               const char *finger_name)
{
  GxdDevice *self = GXD_DEVICE (dbus);
  g_autoptr(GError) error = NULL;
  g_autoptr(Session) session = NULL;
  int finger = gxd_finger_from_name (finger_name);

  if (!check_claimed (self, invocation, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  if (finger == GXD_FINGER_UNKNOWN)
    {
      g_dbus_method_invocation_return_error_literal (invocation, GXD_ERROR,
                                                     GXD_ERROR_INVALID_FINGERNAME,
                                                     "Invalid finger name");
      return TRUE;
    }
  if (!can_start_action (self, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  session = session_get (self);
  if (!delete_fingers (session->username, finger, &error))
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    fprint_dbus_device_complete_delete_enrolled_finger (dbus, invocation);
  return TRUE;
}

/* ---- GObject ---- */

static void
fprint_device_iface_init (FprintDBusDeviceIface *iface)
{
  iface->handle_claim = handle_claim;
  iface->handle_release = handle_release;
  iface->handle_verify_start = handle_verify_start;
  iface->handle_verify_stop = handle_verify_stop;
  iface->handle_enroll_start = handle_enroll_start;
  iface->handle_enroll_stop = handle_enroll_stop;
  iface->handle_list_enrolled_fingers = handle_list_enrolled_fingers;
  iface->handle_delete_enrolled_fingers = handle_delete_enrolled_fingers;
  iface->handle_delete_enrolled_fingers2 = handle_delete_enrolled_fingers2;
  iface->handle_delete_enrolled_finger = handle_delete_enrolled_finger;
}

static void
gxd_device_dispose (GObject *object)
{
  GxdDevice *self = GXD_DEVICE (object);

  g_clear_pointer (&self->clients, g_hash_table_unref);
  if (self->sensor)
    g_signal_handlers_disconnect_by_data (self->sensor, self);
  G_OBJECT_CLASS (gxd_device_parent_class)->dispose (object);
}

static void
gxd_device_finalize (GObject *object)
{
  GxdDevice *self = GXD_DEVICE (object);

  op_reset (self);
  session_set (self, NULL);
  g_clear_object (&self->sensor);
  g_clear_object (&self->auth);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (gxd_device_parent_class)->finalize (object);
}

static void
gxd_device_class_init (GxdDeviceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gxd_device_dispose;
  object_class->finalize = gxd_device_finalize;
  quark_auth_user = g_quark_from_static_string ("gxd-authorized-user");
}

static void
gxd_device_init (GxdDevice *self)
{
  const char *learn = g_getenv ("GOODIX_LEARN");

  g_mutex_init (&self->lock);
  g_queue_init (&self->backlog);
  self->clients = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, unwatch);
  self->learn = !(learn && g_str_equal (learn, "0"));
  g_signal_connect (self, "g-authorize-method", G_CALLBACK (on_authorize), NULL);
}

GxdDevice *
gxd_device_new (GxdSensor *sensor, GError **error)
{
  GxdDevice *self = g_object_new (GXD_TYPE_DEVICE, NULL);
  FprintDBusDevice *dbus = FPRINT_DBUS_DEVICE (self);

  self->auth = polkit_authority_get_sync (NULL, error);
  if (!self->auth)
    {
      g_object_unref (self);
      return NULL;
    }
  self->sensor = g_object_ref (sensor);
  g_signal_connect (sensor, "scan", G_CALLBACK (on_scan), self);
  g_signal_connect (sensor, "finger", G_CALLBACK (on_finger), self);
  g_signal_connect (sensor, "lost", G_CALLBACK (on_lost), self);

  fprint_dbus_device_set_name (dbus, GXD_DEVICE_NAME);
  fprint_dbus_device_set_scan_type (dbus, "press");
  set_enroll_stages (self, ENROLL_MAX_STAGES);
  fprint_dbus_device_set_finger_present (dbus, FALSE);
  fprint_dbus_device_set_finger_needed (dbus, FALSE);
  if (!self->learn)
    g_message ("Print learning disabled (GOODIX_LEARN=0)");
  return self;
}
