====================================
Standalone experimental EEVDF class
====================================

This tree retains the Linux 6.5 CFS implementation and adds an independent
``eevdf_sched_class`` in ``kernel/sched/eevdf.c``. Both policies are available
in one running kernel, in this order::

    stop > deadline > rt > fair > eevdf > idle

This is an experimental, flat EEVDF implementation for CPU-affinity-controlled
experiments, not an exact backport of the complete Linux 6.6 fair scheduler.
Its weighted virtual-time accounting, bounded sleeping lag, lag-preserving
placement and augmented red-black tree search follow the v6.6 implementation.
It selects the earliest virtual deadline among eligible tasks. It does not
import the v6.6 RUN_TO_PARITY or PLACE_DEADLINE_INITIAL heuristics, or later
EEVDF deferred-dequeue behavior. Report this distinction in comparisons with
upstream EEVDF.

Each CPU has an independent ``rq->eevdf``. Each task has separate EEVDF state;
``p->se`` continues to hold common execution accounting and the CFS state.
The running EEVDF entity is excluded from the tree but included in weighted
virtual time. Tree augmentation permits O(log n) eligible-deadline selection.
Virtual-time comparisons use signed differences to handle counter wrapping.

Interface
=========

``SCHED_EEVDF`` is policy number 7 in this tree. This is a **private Linux 6.5
ABI**, not an upstream policy number: later kernels use number 7 for
SCHED_EXT. Use binaries built for this tree only with the corresponding kernel.
Entering the policy from another policy requires CAP_SYS_NICE because this
class does not enforce CPU cgroup bandwidth limits. Existing ownership and
nice/rlimit checks still apply.

Both ``sched_setscheduler()`` and ``sched_setattr()`` select the policy.
``sched_priority`` must be zero. ``sched_setattr()`` accepts:

* ``sched_nice``: the usual -20 through 19 Linux nice weights.
* ``sched_runtime``: requested slice in nanoseconds. Zero selects 750,000 ns.
  Nonzero values must be between 100,000 and 100,000,000 ns, inclusive;
  out-of-range values return EINVAL.
* ``sched_deadline`` and ``sched_period``: must be zero.

For example, using this tree's UAPI headers::

    struct sched_attr attr = {
        .size = sizeof(attr),
        .sched_policy = SCHED_EEVDF,
        .sched_nice = 0,
        .sched_runtime = 200000,  /* 200 us */
    };
    syscall(SYS_sched_setattr, 0, &attr, 0);

Check the syscall return value. These interfaces address a thread (TID),
not automatically every thread in a process. Select the policy before creating
workers, or configure each existing worker explicitly.

``sched_getattr()`` returns the effective requested slice, including the
default. Changing only the slice on an existing EEVDF task takes effect on
re-enqueue; a sleeping task uses it on its next wakeup. A changed slice starts
a new request without resetting accumulated service lag. Legacy
``sched_setparam()`` and ``sched_setscheduler()`` preserve an existing EEVDF
task's slice. New entrants using those legacy interfaces get the default.
Nice changes preserve weighted lag and rescale the remaining virtual request.

Fork/clone inherit policy, nice and slice but initialize a fresh entity with
zero lag. Exec preserves the policy and slice. SCHED_RESET_ON_FORK resets an
EEVDF child to SCHED_NORMAL, nice zero and the default slice. RT mutex priority
inheritance temporarily uses the RT class and restores the EEVDF class when
the boost is removed.

Timing and isolation
====================

The periodic scheduler tick enforces eligibility and request renewal.
High-resolution slice expiration is also supported with CONFIG_SCHED_HRTICK
and the shared HRTICK scheduler feature. With CONFIG_SCHED_DEBUG and debugfs::

    mount -t debugfs none /sys/kernel/debug
    echo HRTICK > /sys/kernel/debug/sched/features

Use the same feature setting for both policies. Without high-resolution
expiration, sub-tick slice values are not precise wall-clock preemption
intervals. Interrupts and non-preemptible kernel execution may delay even a
high-resolution expiration. nohz_full cannot remove the scheduling tick when
multiple ordinary runnable tasks compete on a CPU; EEVDF uses the core
``add_nr_running()``/``sub_nr_running()`` accounting and tick dependencies.
EEVDF also keeps the tick when core scheduling is active, since a lone local
task can compete with an incompatible task on its SMT sibling.

In an EEVDF experiment, put both the latency-sensitive workers and the
background workers in SCHED_EEVDF. A runnable CFS worker has strict priority
over all EEVDF workers, regardless of nice. Keep controllers, load generators
and movable system work on housekeeping CPUs. Record residual scheduler and
IRQ/softirq interference; isolcpus does not eliminate it. Leave the SMT sibling
idle or disable SMT for single-physical-core measurements.

Scope and diagnostics
=====================

CPU affinity, cpuset placement and core CPU-hotplug fallback are supported.
Wakeups retain the previous CPU when permitted. There is no automatic
load balancing: spread or pin workers explicitly. Lag is transferred rather
than comparing absolute virtual times from different CPUs during migration.

CPU cgroup weights/quotas, hierarchical scheduling, NUMA balancing and PELT
utilization tracking are not implemented for this class. Common task and
cgroup CPU-time accounting still runs. Use a controlled CPU frequency for
experiments; schedutil does not have EEVDF utilization estimates. Utilization
clamps are not implemented; EEVDF requests with SCHED_FLAG_UTIL_CLAMP flags
return EOPNOTSUPP. CONFIG_SCHED_CORE has separate class ordering, deadline
comparison, forced-idle virtual-time normalization and eligibility filtering
for this class. Its generic cookie-balancing mechanism can still migrate
tasks within their allowed CPU masks.

With CONFIG_SCHED_DEBUG, ``/proc/PID/sched`` shows ``eevdf.slice``,
``eevdf.vruntime``, ``eevdf.deadline`` and ``eevdf.vlag`` for EEVDF tasks.
The last field is the lag snapshot at dequeue, not an instantaneous lag while
running. ``/sys/kernel/debug/sched/debug`` also lists each EEVDF runqueue.
Runtime, wait and sleep tracepoints and schedstats use the common scheduler
accounting helpers. Enable schedstats when collecting those statistics.

Validation
==========

Build and boot the modified kernel in a VM or on a test machine. The selftest
changes only its own processes/threads. Run it with CAP_SYS_NICE, preferably
with two available CPUs, once with HRTICK disabled and once enabled::

    make O=/tmp/eevdf-build headers
    make -C tools/testing/selftests/sched_eevdf \
        O=/tmp/eevdf-build OUTPUT=/tmp/eevdf-tests/
    sudo /tmp/eevdf-tests/eevdf

With CONFIG_SCHED_CORE, also run ``eevdf --core`` with two SMT siblings
available to exercise cookie selection and service during forced idle.
The test prints its controller and worker CPUs. After booting with isolcpus,
explicitly include the isolated CPU in the launcher's affinity, for example
``sudo taskset -c 0,1 /tmp/eevdf-tests/eevdf`` for housekeeping CPU 0 and test
CPU 1. Otherwise the inherited mask can exclude the isolated CPU entirely.

The test covers parameter validation, slice-only updates, KEEP flags, nice,
fork/exec/reset inheritance, policy transitions, affinity migration, yield,
equal and unequal weighted CPU shares, sleeping tasks, RT mutex PI restoration
and the strict CFS-over-EEVDF priority relationship, plus 24-task enqueue,
sleep, reweight and class-switch stress. It uses broad tolerances
for CPU-share checks and is a correctness smoke test, not a latency benchmark.
