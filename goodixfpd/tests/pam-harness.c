/* pam-harness.c - esegue pam_authenticate con una configurazione PAM di
 * prova (pam_start_confdir), senza root e senza toccare /etc/pam.d.
 *
 *   pam-harness <servizio> <utente> <dir-config>
 *
 * I messaggi del modulo escono su stdout ("info: ..." / "error: ..."), poi
 * "result: <PAM_*>". Exit 0 solo con PAM_SUCCESS.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <security/pam_appl.h>

static int
conv (int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
  for (int i = 0; i < n; i++)
    printf ("%s: %s\n", msg[i]->msg_style == PAM_ERROR_MSG ? "error" : "info", msg[i]->msg);
  fflush (stdout);
  /* solo messaggi informativi/di errore: nessuna risposta da restituire */
  *resp = NULL;
  return PAM_SUCCESS;
}

int
main (int argc, char **argv)
{
  struct pam_conv pc = { conv, NULL };
  pam_handle_t *pamh = NULL;
  int r;

  if (argc != 4)
    {
      fprintf (stderr, "usage: %s SERVICE USER CONFDIR\n", argv[0]);
      return 2;
    }
  r = pam_start_confdir (argv[1], argv[2], &pc, argv[3], &pamh);
  if (r != PAM_SUCCESS)
    {
      fprintf (stderr, "pam_start_confdir: %d\n", r);
      return 2;
    }
  r = pam_authenticate (pamh, 0);
  printf ("result: %s\n", r == PAM_SUCCESS ? "PAM_SUCCESS" :
          r == PAM_MAXTRIES ? "PAM_MAXTRIES" :
          r == PAM_AUTHINFO_UNAVAIL ? "PAM_AUTHINFO_UNAVAIL" :
          r == PAM_AUTH_ERR ? "PAM_AUTH_ERR" : pam_strerror (pamh, r));
  fflush (stdout);
  pam_end (pamh, r);
  return r == PAM_SUCCESS ? 0 : 1;
}
