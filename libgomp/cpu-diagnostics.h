/* This file is part of the GNU Offloading and Multi Processing Library
   (libgomp).

   Libgomp is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   Libgomp is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
   FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
   more details.

   Under Section 7 of GPL version 3, you are granted additional
   permissions described in the GCC Runtime Library Exception, version
   3.1, as published by the Free Software Foundation.

   You should have received a copy of the GNU General Public License and
   a copy of the GCC Runtime Library Exception along with this program;
   see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
   <http://www.gnu.org/licenses/>.  */

#ifndef GOMP_CPU_DIAGNOSTICS_H
#define GOMP_CPU_DIAGNOSTICS_H

enum gomp_cpu_diag_stage
{
  GOMP_CPU_OUTSIDE, GOMP_CPU_RUNTIME, GOMP_CPU_WORK, GOMP_CPU_SPIN,
  GOMP_CPU_FUTEX, GOMP_CPU_STAGE_COUNT
};

struct gomp_cpu_diag_scope
{
  struct gomp_cpu_diag_scope *parent;
  struct gomp_team *team;
  uint64_t start_ns, exclusive_cpu_ns;
  enum gomp_cpu_diag_stage stage;
  bool active;
};

#if defined(__linux__) && defined(LIBGOMP_USE_PTHREADS)
extern bool gomp_cpu_diag_enabled;
extern void gomp_cpu_diag_begin (struct gomp_cpu_diag_scope *,
				enum gomp_cpu_diag_stage);
extern void gomp_cpu_diag_end (struct gomp_cpu_diag_scope *);
extern void gomp_cpu_diag_worker (void);
extern void gomp_cpu_diag_team_begin (struct gomp_team *);
extern void gomp_cpu_diag_team_end (struct gomp_team *);
#else
#define gomp_cpu_diag_enabled false
static inline void
gomp_cpu_diag_begin (struct gomp_cpu_diag_scope *scope,
		     enum gomp_cpu_diag_stage stage)
{
  scope->active = false;
}
static inline void
gomp_cpu_diag_end (struct gomp_cpu_diag_scope *scope) {}
static inline void
gomp_cpu_diag_worker (void) {}
static inline void
gomp_cpu_diag_team_begin (struct gomp_team *team) {}
static inline void
gomp_cpu_diag_team_end (struct gomp_team *team) {}
#endif

static inline void
gomp_cpu_diag_call (void (*fn) (void *), void *data)
{
  if (__builtin_expect (gomp_cpu_diag_enabled, 0))
    {
      struct gomp_cpu_diag_scope scope = { 0 };
      gomp_cpu_diag_begin (&scope, GOMP_CPU_WORK);
      fn (data);
      gomp_cpu_diag_end (&scope);
    }
  else
    fn (data);
}

#endif
