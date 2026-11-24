# Cold-load CPU diagnostics

This experimental branch instruments the libgomp bundled with the qualified
KAITO 0.7.1 / Torch 2.14.0 Trixie runtime. It does not replace the baseline
with upstream GCC master or Debian's system libgomp.

## Source baseline

- GCC upstream revision: `a3253c88425835d5b339d6998a1110a66ccd8b44`.
- AlmaLinux GCC packaging revision: `e76ef486546323c4625ccfa885c26dbe98e6b194`.
- Matched package: `libgomp-8.5.0-28.el8_10.alma.1.x86_64`.
- Bundled GNU build ID: `2244452c1de4759ccb52adc5352b501d4c2ee9ce`.
- Bundled executable `.text` SHA-256:
  `0fdfa2dc24f4f9b288387fca22742aefaad5994bca10ecc1cf7920e0b5c6f53a`.

The vendor libgomp patches, including the 2019 OpenMP backport and its
`include/gomp-constants.h` changes, preserve the bundled OpenMP/OpenACC ABI.
Two build-only compatibility fixes provide syscall prototypes to GCC 14
configure probes and initialize `start_data` for GCC 14's flow analysis.
The build uses Trixie glibc 2.41, the Linux/x86 futex backend, and the Fortran
wrappers needed to retain all bundled exported symbols.

## Enable and interpret

Set `LIBGOMP_CPU_DIAGNOSTICS=1`; diagnostics are disabled otherwise.
No `OMP_*` or `GOMP_*` setting, thread count, affinity, spin count, or wait
policy is changed by the instrumentation.

`CLOCK_THREAD_CPUTIME_ID` measures each calling thread. Stage CPU counters
are exclusive across nested scopes:

| Counter | Meaning |
| --- | --- |
| `work_cpu_ns` | Outlined OpenMP callbacks and task bodies, including application/tensor work; **not pure libgomp instructions**. |
| `spin_cpu_ns` | The existing polling loop, timed outside its iterations. |
| `futex_cpu_ns` | Host CPU spent in the existing futex wait, distinct from its blocked wall time. |
| `runtime_cpu_ns` | Instrumented master orchestration and worker runtime outside nested work/waits. |
| `outside_cpu_ns` | Registered-thread CPU outside these scopes; **not attributable to libgomp**. |
| `observer_cpu_ns`, `observer_wall_ns` | Diagnostic formatting/write costs. Boundary clocks and counter bookkeeping still contribute overhead to the other stages. |

Samples are emitted at scope boundaries, at most once per second per thread,
and at shutdown. Long blocked intervals keep their actual sample boundaries;
do not interpolate them as exact fixed-bucket CPU rates. Final
`snapshot=last_boundary` rows preserve idle workers' last accounted boundary,
not a fresh live-thread measurement. Complement them with external
thread/cgroup CPU sampling.

Team records provide creator TID, sequence, start/end timestamps, and summed
exclusive callback CPU across participating threads. Match creator PID/TID
and intervals to RunAI clone spans. Do not add overlapping clone caller CPU
to the team's CPU. The legacy split `GOMP_parallel_start/end` ABI cannot
observe the master's callback outside the library; its worker callbacks
remain observable.

There is no observer thread and no clock call inside a spin iteration.
Mirrored snapshots are bounded to 1,024 registered threads per process;
overflow, clock, formatting, write, and unstable-final-snapshot failures are
explicit diagnostic errors and invalidate the measurement.
Fork resets diagnostic counters; it does not make inherited OpenMP worker
pools fork-safe. Diagnostics do not synchronize CUDA operations.

## Completed-call latency

The Linux `gomp_team_barrier_wait_final` wrapper is timed around its complete
final-barrier operation, including the last-arrival fast path. Under PASSIVE,
its existing `do_wait` uses the futex path; task handling remains included.
This is final-barrier caller wall latency, not pure futex time or callback time.
Two monotonic clocks surround the high-level call, never a spin iteration.
The existing diagnostics switch enables this instrumentation as well.

`LIBGOMP_LATENCY_SAMPLE` pools completed calls across threads into native
one-second completion windows. Each row contains the actual count and sum of
complete wall durations; a wait crossing a window belongs to its completion
window, not to a sampled unfinished wait age. Empty windows are retained.
Windows start at the first instrumented entry. Explicit quiescent flushes
emit full windows; library shutdown emits the final partial and
`LIBGOMP_LATENCY_END`. The end record declares policy, totals, active calls,
errors, and the bounded 4,096-window capacity. Loss or active calls invalidate
qualification. Fork resets these counters too. Formatting stays outside calls.

## Validation

Reusable commands live in the canonical `v2-test-utils` checkout:

```bash
bash scripts/vllm-gpu-testing/cold-cpu-diagnostics/build-libgomp.sh \
  /path/to/gcc-cpu-diag /path/to/new/evidence-directory
bash scripts/vllm-gpu-testing/cold-cpu-diagnostics/test-libgomp.sh \
  /path/to/gcc-cpu-diag /path/to/evidence-directory
```

Builds and tests use disposable Trixie containers and never install the
candidate into a published runtime. Checks cover the bundled export ABI,
required libc versions, loaded-library binding, real Torch owned tensors in
four dtypes, native ownership/barriers/tasks/nesting, fork reset, disabled
diagnostics, final worker retention, and exact exclusive CPU sums.
