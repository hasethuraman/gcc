/* { dg-do run } */
/* { dg-options "-O2 -pthread" } */

/* Exercise ownership, nested callbacks, tasks, and waits without changing
   the runtime's OpenMP environment or wait policy. */

#include <assert.h>
#include <errno.h>
#include <omp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void
serial_work (void)
{
  volatile uint64_t value = 1;
  unsigned i;
#pragma omp parallel num_threads(1)
  {
    for (i = 0; i < 2000000; i++)
      value = value * 1664525 + 1013904223;
  }
  assert (value != 1);
}

static void
parallel_work (void)
{
  const size_t bytes = 32 * 1024 * 1024;
  unsigned char *source = malloc (bytes), *owned = malloc (bytes);
  unsigned iteration;
  int total = 0;
  assert (source && owned);
  memset (source, 37, bytes);
  for (iteration = 0; iteration < 20; iteration++)
    {
#pragma omp parallel num_threads(2)
      {
	size_t start = omp_get_thread_num () * bytes / 2;
	memcpy (owned + start, source + start, bytes / 2);
	if (omp_get_thread_num () == 0 && iteration == 0)
	  usleep (100000);
#pragma omp barrier
      }
      assert (memcmp (source, owned, bytes) == 0);
    }
  memset (source, 0, bytes);
  assert (owned[0] == 37 && owned[bytes - 1] == 37);
#pragma omp parallel num_threads(2)
  {
#pragma omp single
    {
      unsigned i;
      for (i = 0; i < 40; i++)
	{
#pragma omp task shared(total)
	  {
#pragma omp atomic update
	    total++;
	  }
	}
#pragma omp taskwait
    }
  }
  assert (total == 40);
#pragma omp parallel num_threads(2)
  {
    /* Default active-level limits may serialize this nested region. */
#pragma omp parallel num_threads(2)
    {
      assert (omp_get_num_threads () >= 1);
    }
  }
  free (source);
  free (owned);
}

int
main (void)
{
  pid_t child;
  int status;
  errno = EAGAIN;
  serial_work ();
  assert (errno == EAGAIN);
  /* Fork before creating worker threads: inherited libgomp pools are not
     made fork-safe by the diagnostic reset. */
  child = fork ();
  assert (child >= 0);
  if (child == 0)
    {
      serial_work ();
      exit (0);
    }
  assert (waitpid (child, &status, 0) == child);
  assert (WIFEXITED (status) && WEXITSTATUS (status) == 0);
  parallel_work ();
  return 0;
}
