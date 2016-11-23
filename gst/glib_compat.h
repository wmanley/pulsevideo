#ifndef __PV_GLIB_COMPAT_H_
#define __PV_GLIB_COMPAT_H_

#include <glib.h>

#ifndef g_steal_pointer
static inline gpointer
(g_steal_pointer) (gpointer pp)
{
  gpointer *ptr = pp;
  gpointer ref;

  ref = *ptr;
  *ptr = NULL;

  return ref;
}

/* type safety */
#define g_steal_pointer(pp) \
  (0 ? (*(pp)) : (g_steal_pointer) (pp))
#endif

#endif  /* __PV_GLIB_COMPAT_H_ */
