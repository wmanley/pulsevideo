/*
 * Copyright (C) 2021 William Manley <will@stb-tester.com>
 *
 * Fault injection system
 * ======================
 *
 * This allows us to inject faults into pulsevideo to deterministically create
 * failures at various points in the system.  It's controlled by environment
 * variables.  So if I set:
 *
 *     fdpay_buffer="skip skip usleep=1000000 skip abort" pulsevideo ...
 *
 * The video stream will pause for 1s on the 3rd buffer and abort on the 4th
 * one.
 *
 * The commands are:
 *
 * * `usleep=<usecs>` - sleep for `<usecs>` microseconds.  Note: unlike with
 *   the other commands, after the sleep the next command will be executed.
 * * `skip` - don't inject a fault on this invocation
 * * `gerror` - return an error
 * * `abort` - crash with SIGABRT
 *
 * The injection points are:
 *
 * * `pre_attach` - called in the handler for the incoming DBus request before
 *   returning the value to the client.  This happens after the socket has
 *   been created and been added to the `multisocketsink`.
 * * `fdpay_buffer` - called in the streaming thread for every buffer to be
 *   sent.
 */

#ifndef __FAULT_H__
#define __FAULT_H__

#include <glib.h>
#include <err.h>

#ifdef ENABLE_FAULT_INJECTION
static const gboolean FAULT_INJECTION = 1;
#else
static const gboolean FAULT_INJECTION = 0;
#endif

struct FaultInjectionPoint {
  const char * const name;
  GMutex mutex;
  char *cmds;
};

#define FAULT_INJECTION_POINT(name) {name, {0}, NULL}

static gboolean
inject_fault (struct FaultInjectionPoint* p, GError **err)
{
  if (!FAULT_INJECTION)
    return TRUE;

  gboolean res = TRUE;

  g_mutex_lock (&p->mutex);
  if (!p->cmds) {
    const gchar *e = g_getenv (p->name);
    p->cmds = g_strdup (e ? e : "");
  }
  while (p->cmds[0] != '\0') {
    if (p->cmds[0] == ' ')
      p->cmds++;
    else if (memcmp ("usleep=", p->cmds, 7) == 0) {
      p->cmds += 7;
      int duration_us = strtol(p->cmds, &p->cmds, 10);
      warnx ("inject_fault %s: sleeping %i us", p->name, duration_us);
      g_mutex_unlock (&p->mutex);
      g_usleep(duration_us);
      g_mutex_lock (&p->mutex);
    } else if (memcmp ("gerror", p->cmds, 6) == 0) {
      p->cmds += 6;
      warnx ("inject_fault %s: return GError", p->name);
      g_set_error (err, G_FILE_ERROR, G_FILE_ERROR_NOENT, "Fault injected from %s", p->name);
      res = FALSE;
      break;
    } else if (memcmp ("abort", p->cmds, 5) == 0) {
      warnx ("inject_fault %s: aborting", p->name);
      raise(SIGABRT);
    } else if (memcmp ("skip", p->cmds, 4) == 0) {
      p->cmds += 4;
      /* This allows us to not error first time */
      warnx ("inject_fault %s: skip", p->name);
      break;
    } else {
      warnx ("inject_fault %s: Ignoring invalid description: %s", p->name, p->cmds);
      p->cmds = "";
      break;
    }
  }
  g_mutex_unlock (&p->mutex);
  return res;
}

#endif /* __FAULT_H__ */