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

#define _GNU_SOURCE
#include "libgomp.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__) && defined(LIBGOMP_USE_PTHREADS)
bool gomp_cpu_diag_enabled;
static pthread_key_t diag_key;
static uint64_t diag_team_sequence, diag_errors;

struct diag_thread
{
  struct gomp_cpu_diag_scope *scope;
  uint64_t cpu[GOMP_CPU_STAGE_COUNT], calls[GOMP_CPU_STAGE_COUNT];
  uint64_t wall[GOMP_CPU_STAGE_COUNT];
  uint64_t last_cpu, first_cpu, first_ns, last_sample, observer_cpu;
  uint64_t observer_wall;
  long tid;
  unsigned slot;
  bool initialized, failed, worker;
};
static __thread struct diag_thread diag;

#define DIAG_SLOTS 1024
#define DIAG_FIELDS (10 + 3 * GOMP_CPU_STAGE_COUNT)
struct diag_snapshot
{
  uint64_t sequence, fields[DIAG_FIELDS];
};
static struct diag_snapshot diag_snapshots[DIAG_SLOTS];
static unsigned diag_slots;

/* Process-owned buffers retain nested teams created by pool workers too.
   Formatting and writes run only at explicit flush or library shutdown. */
#define DIAG_RECORDS 65536
struct diag_sample_record
{
  uint64_t fields[DIAG_FIELDS];
  bool final, ready;
};
struct diag_team_record
{
  uint64_t creator, sequence, threads, start, end, work;
  bool ready;
};
static struct diag_sample_record diag_samples[DIAG_RECORDS];
static struct diag_team_record diag_teams[DIAG_RECORDS];
static unsigned diag_sample_count, diag_team_count;
static unsigned diag_samples_emitted, diag_teams_emitted;
static pthread_mutex_t diag_dump_lock = PTHREAD_MUTEX_INITIALIZER;

static void
diag_error (const char *operation, int error)
{
  char line[192];
  int n;
  __atomic_add_fetch (&diag_errors, 1, __ATOMIC_RELAXED);
  n = snprintf (line, sizeof line,
		"LIBGOMP_CPU_ERROR operation=%s error=%d pid=%ld\n",
		operation, error, (long) getpid ());
  if (n > 0 && (size_t) n < sizeof line)
    {
      ssize_t ignored = write (STDERR_FILENO, line, n);
      (void) ignored;
    }
  diag.failed = true;
}

static uint64_t
diag_clock (clockid_t clock)
{
  struct timespec value;
  if (clock_gettime (clock, &value) != 0)
    {
      diag_error ("clock_gettime", errno);
      return 0;
    }
  return (uint64_t) value.tv_sec * 1000000000ULL + value.tv_nsec;
}

static void
diag_write (const char *line, int size, size_t capacity)
{
  ssize_t written;
  if (size <= 0 || (size_t) size >= capacity)
    {
      diag_error ("format", EOVERFLOW);
      return;
    }
  do
    written = write (STDERR_FILENO, line, size);
  while (written < 0 && errno == EINTR);
  if (written != size)
    diag_error ("write", written < 0 ? errno : EIO);
}

static void
diag_account (uint64_t cpu)
{
  enum gomp_cpu_diag_stage stage;
  uint64_t delta;
  if (diag.failed)
    return;
  if (cpu < diag.last_cpu)
    {
      diag_error ("thread_clock_regressed", EOVERFLOW);
      return;
    }
  delta = cpu - diag.last_cpu;
  stage = diag.scope ? diag.scope->stage : GOMP_CPU_OUTSIDE;
  diag.cpu[stage] += delta;
  if (diag.scope)
    diag.scope->exclusive_cpu_ns += delta;
  diag.last_cpu = cpu;
}

/* Mirrors contain atomic values, not pointers into another thread's TLS.
   They retain the last completed boundary even when process exit does not
   run pthread destructors for idle pool workers. */
static void
diag_publish (uint64_t now, uint64_t cpu)
{
  struct diag_snapshot *snapshot;
  uint64_t fields[DIAG_FIELDS];
  unsigned i;
  if (diag.failed || !diag.initialized)
    return;
  snapshot = &diag_snapshots[diag.slot];
  fields[0] = diag.tid;
  fields[1] = diag.first_ns;
  fields[2] = diag.last_sample;
  fields[3] = now;
  fields[4] = cpu - diag.first_cpu;
  fields[5] = diag.observer_cpu;
  fields[6] = diag.worker;
  fields[7] = diag.scope ? diag.scope->stage : GOMP_CPU_OUTSIDE;
  fields[8] = __atomic_load_n (&diag_errors, __ATOMIC_RELAXED);
  fields[9 + 3 * GOMP_CPU_STAGE_COUNT] = diag.observer_wall;
  for (i = 0; i < GOMP_CPU_STAGE_COUNT; i++)
    {
      fields[9 + i] = diag.cpu[i];
      fields[9 + GOMP_CPU_STAGE_COUNT + i] = diag.calls[i];
      fields[9 + 2 * GOMP_CPU_STAGE_COUNT + i] = diag.wall[i];
    }
  __atomic_add_fetch (&snapshot->sequence, 1, __ATOMIC_SEQ_CST);
  for (i = 0; i < DIAG_FIELDS; i++)
    __atomic_store_n (&snapshot->fields[i], fields[i], __ATOMIC_RELAXED);
  __atomic_add_fetch (&snapshot->sequence, 1, __ATOMIC_RELEASE);
}

static void
diag_emit (uint64_t *f, bool final, const char *kind)
{
  char line[1536];
  uint64_t *cpu = f + 9;
  uint64_t *calls = cpu + GOMP_CPU_STAGE_COUNT;
  uint64_t *wall = calls + GOMP_CPU_STAGE_COUNT;
  int n = snprintf (line, sizeof line,
	"LIBGOMP_CPU_SAMPLE version=1 pid=%ld tid=%llu worker=%llu "
	"first_ns=%llu previous_ns=%llu monotonic_ns=%llu "
	"thread_cpu_ns=%llu outside_cpu_ns=%llu runtime_cpu_ns=%llu "
	"work_cpu_ns=%llu spin_cpu_ns=%llu futex_cpu_ns=%llu "
	"observer_cpu_ns=%llu work_calls=%llu spin_calls=%llu "
	"futex_calls=%llu work_wall_ns=%llu spin_wall_ns=%llu "
	"futex_wall_ns=%llu observer_wall_ns=%llu "
	"active_stage=%llu final=%d errors=%llu snapshot=%s\n",
	(long) getpid (), (unsigned long long) f[0], (unsigned long long) f[6],
	(unsigned long long) f[1], (unsigned long long) f[2],
	(unsigned long long) f[3], (unsigned long long) f[4],
	(unsigned long long) cpu[GOMP_CPU_OUTSIDE],
	(unsigned long long) cpu[GOMP_CPU_RUNTIME],
	(unsigned long long) cpu[GOMP_CPU_WORK],
	(unsigned long long) cpu[GOMP_CPU_SPIN],
	(unsigned long long) cpu[GOMP_CPU_FUTEX],
	(unsigned long long) f[5],
	(unsigned long long) calls[GOMP_CPU_WORK],
	(unsigned long long) calls[GOMP_CPU_SPIN],
	(unsigned long long) calls[GOMP_CPU_FUTEX],
	(unsigned long long) wall[GOMP_CPU_WORK],
	(unsigned long long) wall[GOMP_CPU_SPIN],
	(unsigned long long) wall[GOMP_CPU_FUTEX],
	(unsigned long long) f[9 + 3 * GOMP_CPU_STAGE_COUNT],
	(unsigned long long) f[7], final, (unsigned long long) f[8], kind);
  diag_write (line, n, sizeof line);
}

/* Buffer at completed boundaries, never inside the polling loop. */
static void
diag_flush (bool final, uint64_t now, uint64_t cpu)
{
  uint64_t after;
  unsigned slot, field;
  struct diag_sample_record *record;
  int saved_errno = errno;
  if (!diag.initialized || diag.failed)
    return;
  if (final)
    {
      now = diag_clock (CLOCK_MONOTONIC);
      cpu = diag_clock (CLOCK_THREAD_CPUTIME_ID);
    }
  diag_account (cpu);
  diag_publish (now, cpu);
  if (!final && now - diag.last_sample < 1000000000ULL)
    {
      errno = saved_errno;
      return;
    }
  slot = __atomic_fetch_add (&diag_sample_count, 1, __ATOMIC_RELAXED);
  if (slot >= DIAG_RECORDS)
    {
      diag_error ("sample_capacity", EOVERFLOW);
      errno = saved_errno;
      return;
    }
  record = &diag_samples[slot];
  for (field = 0; field < DIAG_FIELDS; field++)
    record->fields[field]
      = __atomic_load_n (&diag_snapshots[diag.slot].fields[field],
			 __ATOMIC_RELAXED);
  record->final = final;
  __atomic_store_n (&record->ready, true, __ATOMIC_RELEASE);
  after = diag_clock (CLOCK_THREAD_CPUTIME_ID);
  if (after >= cpu)
    diag.observer_cpu += after - cpu;
  diag.observer_wall += diag_clock (CLOCK_MONOTONIC) - now;
  diag.last_cpu = after;
  diag.last_sample = now;
  errno = saved_errno;
}

void
gomp_cpu_diag_begin (struct gomp_cpu_diag_scope *scope,
		     enum gomp_cpu_diag_stage stage)
{
  uint64_t now, cpu;
  int saved_errno = errno;
  scope->active = false;
  if (!gomp_cpu_diag_enabled || diag.failed)
    return;
  now = diag_clock (CLOCK_MONOTONIC);
  cpu = diag_clock (CLOCK_THREAD_CPUTIME_ID);
  if (!diag.initialized)
    {
      diag.first_ns = diag.last_sample = now;
      diag.first_cpu = diag.last_cpu = cpu;
      diag.tid = syscall (SYS_gettid);
      diag.slot = __atomic_fetch_add (&diag_slots, 1, __ATOMIC_RELAXED);
      if (diag.slot >= DIAG_SLOTS)
	{
	  diag_error ("thread_capacity", EOVERFLOW);
	  errno = saved_errno;
	  return;
	}
      diag.initialized = true;
      if (pthread_setspecific (diag_key, (void *) 1) != 0)
	diag_error ("pthread_setspecific", EIO);
    }
  diag_account (cpu);
  scope->parent = diag.scope;
  scope->stage = stage;
  scope->start_ns = now;
  scope->exclusive_cpu_ns = 0;
  scope->team = stage == GOMP_CPU_WORK ? gomp_thread ()->ts.team : NULL;
  scope->active = !diag.failed;
  if (scope->active)
    diag.scope = scope;
  errno = saved_errno;
}

void
gomp_cpu_diag_end (struct gomp_cpu_diag_scope *scope)
{
  uint64_t cpu, now;
  int saved_errno = errno;
  if (!scope->active)
    return;
  cpu = diag_clock (CLOCK_THREAD_CPUTIME_ID);
  now = diag_clock (CLOCK_MONOTONIC);
  if (diag.scope != scope)
    diag_error ("scope_order", EINVAL);
  diag_account (cpu);
  diag.calls[scope->stage]++;
  diag.wall[scope->stage] += now - scope->start_ns;
  if (scope->stage == GOMP_CPU_WORK && scope->team && !diag.failed)
    __atomic_add_fetch (&scope->team->cpu_diag_work,
			scope->exclusive_cpu_ns, __ATOMIC_RELAXED);
  diag.scope = scope->parent;
  scope->active = false;
  diag_flush (false, now, cpu);
  errno = saved_errno;
}

void
gomp_cpu_diag_worker (void)
{
  diag.worker = true;
}

void
gomp_cpu_diag_team_begin (struct gomp_team *team)
{
  int saved_errno = errno;
  if (gomp_cpu_diag_enabled)
    {
      team->cpu_diag_sequence
	= __atomic_add_fetch (&diag_team_sequence, 1, __ATOMIC_RELAXED);
      team->cpu_diag_creator = syscall (SYS_gettid);
      team->cpu_diag_start = diag_clock (CLOCK_MONOTONIC);
      team->cpu_diag_work = 0;
    }
  errno = saved_errno;
}

void
gomp_cpu_diag_team_end (struct gomp_team *team)
{
  uint64_t before, after, start;
  struct gomp_cpu_diag_scope initial_scope = { 0 };
  struct diag_team_record *record;
  unsigned slot;
  int saved_errno = errno;
  if (!gomp_cpu_diag_enabled || diag.failed)
    return;
  if (!diag.initialized)
    gomp_cpu_diag_begin (&initial_scope, GOMP_CPU_RUNTIME);
  start = diag_clock (CLOCK_MONOTONIC);
  before = diag_clock (CLOCK_THREAD_CPUTIME_ID);
  diag_account (before);
  slot = __atomic_fetch_add (&diag_team_count, 1, __ATOMIC_RELAXED);
  if (slot >= DIAG_RECORDS)
    {
      diag_error ("team_capacity", EOVERFLOW);
      errno = saved_errno;
      return;
    }
  record = &diag_teams[slot];
  record->creator = team->cpu_diag_creator;
  record->sequence = team->cpu_diag_sequence;
  record->threads = team->nthreads;
  record->start = team->cpu_diag_start;
  record->end = diag_clock (CLOCK_MONOTONIC);
  record->work = __atomic_load_n (&team->cpu_diag_work, __ATOMIC_RELAXED);
  __atomic_store_n (&record->ready, true, __ATOMIC_RELEASE);
  after = diag_clock (CLOCK_THREAD_CPUTIME_ID);
  if (after >= before)
    diag.observer_cpu += after - before;
  diag.observer_wall += diag_clock (CLOCK_MONOTONIC) - start;
  diag.last_cpu = after;
  gomp_cpu_diag_end (&initial_scope);
  errno = saved_errno;
}

static void
diag_thread_exit (void *unused)
{
  diag_flush (true, 0, 0);
}

static void
diag_child (void)
{
  memset (&diag, 0, sizeof diag);
  memset (diag_snapshots, 0, sizeof diag_snapshots);
  memset (diag_samples, 0, sizeof diag_samples);
  memset (diag_teams, 0, sizeof diag_teams);
  diag_slots = 0;
  diag_team_sequence = diag_errors = 0;
  diag_sample_count = diag_team_count = 0;
  diag_samples_emitted = diag_teams_emitted = 0;
  {
    pthread_mutex_t fresh = PTHREAD_MUTEX_INITIALIZER;
    memcpy (&diag_dump_lock, &fresh, sizeof fresh);
  }
}

static void __attribute__((constructor))
diag_initialize (void)
{
  const char *value = getenv ("LIBGOMP_CPU_DIAGNOSTICS");
  int error;
  if (!value || strcmp (value, "1") != 0)
    return;
  error = pthread_key_create (&diag_key, diag_thread_exit);
  if (!error)
    error = pthread_atfork (NULL, NULL, diag_child);
  if (error)
    {
      diag_error ("initialize", error);
      return;
    }
  gomp_cpu_diag_enabled = true;
}

/* Called outside streaming operations, before a worker can be terminated. */
void __attribute__((visibility ("default")))
GOMP_cpu_diagnostics_flush (void)
{
  int saved_errno = errno;
  if (gomp_cpu_diag_enabled)
    {
      uint64_t cpu, now, after;
      unsigned i, count = __atomic_load_n (&diag_slots, __ATOMIC_ACQUIRE);
      unsigned records;
      pthread_mutex_lock (&diag_dump_lock);
      diag_flush (true, 0, 0);
      cpu = diag_clock (CLOCK_THREAD_CPUTIME_ID);
      if (diag.initialized)
	diag_account (cpu);
      now = diag_clock (CLOCK_MONOTONIC);
      records = __atomic_load_n (&diag_sample_count, __ATOMIC_ACQUIRE);
      if (records > DIAG_RECORDS)
	records = DIAG_RECORDS;
      for (i = diag_samples_emitted; i < records; i++)
	{
	  struct diag_sample_record *record = &diag_samples[i];
	  if (!__atomic_load_n (&record->ready, __ATOMIC_ACQUIRE))
	    break;
	  diag_emit (record->fields, record->final, "boundary");
	  diag_samples_emitted = i + 1;
	}
      records = __atomic_load_n (&diag_team_count, __ATOMIC_ACQUIRE);
      if (records > DIAG_RECORDS)
	records = DIAG_RECORDS;
      for (i = diag_teams_emitted; i < records; i++)
	{
	  struct diag_team_record *record = &diag_teams[i];
	  char line[512];
	  int n;
	  if (!__atomic_load_n (&record->ready, __ATOMIC_ACQUIRE))
	    break;
	  n = snprintf (line, sizeof line,
	    "LIBGOMP_CPU_TEAM version=1 pid=%ld creator_tid=%llu team=%llu "
	    "threads=%llu start_ns=%llu end_ns=%llu work_cpu_ns=%llu\n",
	    (long) getpid (), (unsigned long long) record->creator,
	    (unsigned long long) record->sequence,
	    (unsigned long long) record->threads,
	    (unsigned long long) record->start,
	    (unsigned long long) record->end,
	    (unsigned long long) record->work);
	  diag_write (line, n, sizeof line);
	  diag_teams_emitted = i + 1;
	}
      if (count > DIAG_SLOTS)
	count = DIAG_SLOTS;
      for (i = 0; i < count; i++)
	{
	  struct diag_snapshot *snapshot = &diag_snapshots[i];
	  uint64_t fields[DIAG_FIELDS], before, after;
	  unsigned attempt, field;
	  for (attempt = 0; attempt < 100; attempt++)
	    {
	      before = __atomic_load_n (&snapshot->sequence, __ATOMIC_ACQUIRE);
	      if (!before || (before & 1))
		continue;
	      for (field = 0; field < DIAG_FIELDS; field++)
		fields[field] = __atomic_load_n (&snapshot->fields[field],
						__ATOMIC_RELAXED);
	      __atomic_thread_fence (__ATOMIC_ACQUIRE);
	      after = __atomic_load_n (&snapshot->sequence, __ATOMIC_RELAXED);
	      if (before == after)
		break;
	    }
	  if (attempt == 100)
	    diag_error ("final_snapshot_busy", EBUSY);
	  else if ((long) fields[0] != diag.tid)
	    diag_emit (fields, true, "last_boundary");
	}
      after = diag_clock (CLOCK_THREAD_CPUTIME_ID);
      if (diag.initialized && after >= cpu)
	{
	  diag.observer_cpu += after - cpu;
	  diag.observer_wall += diag_clock (CLOCK_MONOTONIC) - now;
	  diag.last_cpu = after;
	}
      pthread_mutex_unlock (&diag_dump_lock);
    }
  errno = saved_errno;
}

static void __attribute__((destructor))
diag_finalize (void)
{
  GOMP_cpu_diagnostics_flush ();
  if (gomp_cpu_diag_enabled)
    {
      if (diag_samples_emitted != diag_sample_count
	  || diag_teams_emitted != diag_team_count)
	diag_error ("unfinished_records", EBUSY);
      pthread_key_delete (diag_key);
    }
}
#endif
