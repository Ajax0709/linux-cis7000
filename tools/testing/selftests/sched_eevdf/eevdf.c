// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../kselftest.h"

/* Private Linux 6.5 ABI. Policy 7 has a different meaning in newer kernels. */
#define SCHED_EEVDF 7
#define RESET_ON_FORK 1
#define KEEP_POLICY 8
#define KEEP_PARAMS 16
#define DEFAULT_SLICE 750000ULL
#define CORE_SCOPE_THREAD 0

struct test_attr {
	uint32_t size, policy;
	uint64_t flags;
	int32_t nice;
	uint32_t priority;
	uint64_t runtime, deadline, period;
	uint32_t util_min, util_max;
};

static int cpu_control, cpu_test;
static const char *self;
static pid_t test_pid;

static void protect_child(void)
{
	/* A failed/timed-out controller must not leave CPU burners behind. */
	if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != test_pid)
		_exit(1);
}

static int set_attr(int policy, int nice, uint64_t slice, uint64_t flags)
{
	struct test_attr attr = {
		.size = sizeof(attr), .policy = policy, .nice = nice,
		.runtime = slice, .flags = flags,
	};

	return syscall(SYS_sched_setattr, 0, &attr, 0);
}

static int get_attr(struct test_attr *attr)
{
	memset(attr, 0, sizeof(*attr));
	return syscall(SYS_sched_getattr, 0, attr, sizeof(*attr), 0);
}

static int pin_cpu(int cpu)
{
	cpu_set_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	return sched_setaffinity(0, sizeof(mask), &mask);
}

static uint64_t now(clockid_t clock)
{
	struct timespec ts;

	clock_gettime(clock, &ts);
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void burn_cpu(uint64_t ns)
{
	uint64_t end = now(CLOCK_THREAD_CPUTIME_ID) + ns;
	unsigned long n = 0;

	do {
		for (int i = 0; i < 1000; i++) {
			n++;
			asm volatile("" : "+r" (n));
		}
	} while (now(CLOCK_THREAD_CPUTIME_ID) < end);
}

static bool child_ok(pid_t pid)
{
	int status;

	return pid > 0 && waitpid(pid, &status, 0) == pid &&
		WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool inheritance(bool reset, bool exec)
{
	struct test_attr attr;
	pid_t pid;
	bool ok;

	if (set_attr(SCHED_EEVDF, 3, 200000, reset ? RESET_ON_FORK : 0))
		return false;
	pid = fork();
	if (!pid) {
		protect_child();
		if (exec) {
			execl(self, self, "--exec-child", NULL);
			_exit(1);
		}
		if (get_attr(&attr))
			_exit(1);
		_exit(reset ? (attr.policy != SCHED_OTHER || attr.flags) :
		      (attr.policy != SCHED_EEVDF || attr.runtime != 200000 ||
		       attr.nice != 3));
	}
	ok = child_ok(pid);
	if (set_attr(SCHED_OTHER, 0, 0, 0))
		ok = false;
	return ok;
}

static bool invalid_params(void)
{
	struct test_attr bad[] = {
		{ .runtime = 99999 }, { .runtime = 100000001 },
		{ .runtime = 200000, .deadline = 1 },
		{ .runtime = 200000, .period = 1 },
		{ .runtime = 200000, .priority = 1 },
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(bad); i++) {
		bad[i].size = sizeof(bad[i]);
		bad[i].policy = SCHED_EEVDF;
		errno = 0;
		if (syscall(SYS_sched_setattr, 0, &bad[i], 0) != -1 || errno != EINVAL)
			return false;
	}
	return true;
}

static bool affinity_test(void)
{
	pid_t pid = fork();

	if (!pid) {
		protect_child();
		if (set_attr(SCHED_EEVDF, 0, 100000, 0))
			_exit(1);
		for (int i = 0; i < 100; i++) {
			int cpu = i & 1 ? cpu_control : cpu_test;

			if (pin_cpu(cpu) || sched_getcpu() != cpu)
				_exit(1);
			burn_cpu(100000);
			sched_yield();
		}
		_exit(sched_getscheduler(0) != SCHED_EEVDF);
	}
	return child_ok(pid);
}

struct pair_result {
	uint64_t end, runtime[2], iterations[2];
};

/* Controller stays on another CPU; both contenders use one test CPU. */
static bool run_pair(int nice_second, bool sleeper, bool mixed, bool cookies)
{
	struct pair_result *result;
	int ready[2], start[2];
	pid_t pids[2] = { -1, -1 };
	bool ok = false;
	char token = 0;

	result = mmap(NULL, sizeof(*result), PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (result == MAP_FAILED)
		return false;
	if (pipe(ready))
		goto unmap;
	if (pipe(start)) {
		close(ready[0]);
		close(ready[1]);
		goto unmap;
	}
	for (int i = 0; i < 2; i++) {
		pids[i] = fork();
		if (pids[i] < 0)
			goto out;
		if (!pids[i]) {
			uint64_t begin;
			int policy = mixed && i == 0 ? SCHED_OTHER : SCHED_EEVDF;

			protect_child();
			close(ready[0]);
			close(start[1]);
			if (pin_cpu(cookies && i == 0 ? cpu_control : cpu_test) ||
			    set_attr(policy, i ? nice_second : 0,
				     policy == SCHED_EEVDF ? (i ? 750000 : 100000) : 0, 0))
				_exit(1);
			if (cookies && prctl(PR_SCHED_CORE, PR_SCHED_CORE_CREATE,
					    0L, CORE_SCOPE_THREAD, 0L))
				_exit(1);
			if (write(ready[1], &token, 1) != 1 || read(start[0], &token, 1) != 1)
				_exit(1);
			begin = now(CLOCK_PROCESS_CPUTIME_ID);
			while (now(CLOCK_MONOTONIC) < result->end) {
				burn_cpu(50000);
				result->iterations[i]++;
				if (sleeper && i == 0)
					usleep(1000);
			}
			result->runtime[i] = now(CLOCK_PROCESS_CPUTIME_ID) - begin;
			_exit(0);
		}
	}
	close(ready[1]);
	ready[1] = -1;
	close(start[0]);
	start[0] = -1;
	for (int i = 0; i < 2; i++)
		if (read(ready[0], &token, 1) != 1)
			goto out;
	result->end = now(CLOCK_MONOTONIC) + 700000000;
	if (write(start[1], "go", 2) != 2)
		goto out;
	ok = child_ok(pids[0]);
	pids[0] = -1;
	ok = child_ok(pids[1]) && ok;
	pids[1] = -1;
	if (ok) {
		double ratio = (double)result->runtime[0] / (result->runtime[1] ?: 1);

		ksft_print_msg("CPU times: %llu / %llu ns, ratio %.3f\n",
			       (unsigned long long)result->runtime[0],
			       (unsigned long long)result->runtime[1], ratio);
		if (mixed)
			ok = ratio > 5 && result->runtime[0] > 50000000;
		else if (sleeper)
			ok = result->iterations[0] > 10 && result->iterations[1] > 10;
		else if (nice_second)
			ok = ratio > 2.0 && ratio < 4.5;
		else
			ok = ratio > 0.65 && ratio < 1.55;
	}
out:
	for (int i = 0; i < 2; i++) {
		if (pids[i] > 0) {
			kill(pids[i], SIGKILL);
			waitpid(pids[i], NULL, 0);
		}
		if (ready[i] >= 0)
			close(ready[i]);
		if (start[i] >= 0)
			close(start[i]);
	}
unmap:
	munmap(result, sizeof(*result));
	return ok;
}

static pthread_mutex_t pi_mutex;
static atomic_int pi_stage;

static void *pi_holder(void *unused)
{
	(void)unused;
	if (pin_cpu(cpu_test) || set_attr(SCHED_EEVDF, 0, 100000, 0) ||
	    pthread_mutex_lock(&pi_mutex)) {
		atomic_store(&pi_stage, -1);
		return (void *)1;
	}
	atomic_store(&pi_stage, 1);
	while (atomic_load(&pi_stage) == 1)
		usleep(1000);
	/* Let the RT waiter block on the mutex, boosting this EEVDF holder. */
	usleep(20000);
	if (pthread_mutex_unlock(&pi_mutex))
		return (void *)1;
	burn_cpu(20000000);
	return (void *)(long)(sched_getscheduler(0) != SCHED_EEVDF);
}

static void *pi_waiter(void *unused)
{
	struct sched_param sp = { .sched_priority = 10 };

	(void)unused;
	if (pin_cpu(cpu_test) || sched_setscheduler(0, SCHED_FIFO, &sp)) {
		atomic_store(&pi_stage, 2);
		return (void *)1;
	}
	atomic_store(&pi_stage, 2);
	if (pthread_mutex_lock(&pi_mutex) || pthread_mutex_unlock(&pi_mutex))
		return (void *)1;
	return NULL;
}

static bool pi_test(void)
{
	pthread_mutexattr_t attr;
	pthread_t holder, waiter;
	void *a, *b;

	if (pthread_mutexattr_init(&attr))
		return false;
	if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) ||
	    pthread_mutex_init(&pi_mutex, &attr)) {
		pthread_mutexattr_destroy(&attr);
		return false;
	}
	pthread_mutexattr_destroy(&attr);
	atomic_store(&pi_stage, 0);
	if (pthread_create(&holder, NULL, pi_holder, NULL))
		return false;
	while (!atomic_load(&pi_stage))
		usleep(1000);
	if (atomic_load(&pi_stage) < 0 ||
	    pthread_create(&waiter, NULL, pi_waiter, NULL)) {
		atomic_store(&pi_stage, 2);
		pthread_join(holder, &a);
		pthread_mutex_destroy(&pi_mutex);
		return false;
	}
	pthread_join(holder, &a);
	pthread_join(waiter, &b);
	pthread_mutex_destroy(&pi_mutex);
	return !a && !b;
}

static bool stress_requeue(void)
{
	pid_t pids[24];
	unsigned int nr = 0;
	bool ok = true;

	for (unsigned int i = 0; i < ARRAY_SIZE(pids); i++) {
		pid_t pid = fork();

		if (pid < 0) {
			ok = false;
			break;
		}
		if (!pid) {
			protect_child();
			if (pin_cpu(cpu_test))
				_exit(1);
			for (int j = 0; j < 60; j++) {
				if (set_attr(SCHED_EEVDF, (i + j) % 10,
					     100000 + 10000 * ((i + j) % 40), 0))
					_exit(1);
				burn_cpu(50000);
				if (j % 3 == 0)
					usleep(1000);
				if (j % 5 == 0)
					sched_yield();
				if (j % 7 == 0 && set_attr(SCHED_OTHER, 0, 0, 0))
					_exit(1);
			}
			_exit(0);
		}
		pids[nr++] = pid;
	}
	for (unsigned int i = 0; i < nr; i++)
		ok = child_ok(pids[i]) && ok;
	return ok;
}

int main(int argc, char **argv)
{
	struct test_attr attr;
	struct sched_param sp = { 0 };
	cpu_set_t allowed;
	bool ok;
	bool core_mode = argc == 2 && !strcmp(argv[1], "--core");

	if (argc == 2 && !strcmp(argv[1], "--exec-child"))
		return get_attr(&attr) || attr.policy != SCHED_EEVDF ||
			attr.runtime != 200000 || attr.nice != 3;
	self = argv[0];
	test_pid = getpid();
	ksft_print_header();
	if (core_mode && prctl(PR_SCHED_CORE, PR_SCHED_CORE_CREATE,
			      0L, CORE_SCOPE_THREAD, 0L))
		ksft_exit_skip("core scheduling unavailable: %s\n", strerror(errno));
	if (sched_get_priority_max(SCHED_EEVDF) < 0)
		ksft_exit_skip("SCHED_EEVDF is not supported by this kernel\n");
	if (sched_getaffinity(0, sizeof(allowed), &allowed))
		ksft_exit_fail_msg("sched_getaffinity: %s\n", strerror(errno));
	cpu_control = cpu_test = -1;
	for (int i = 0; i < CPU_SETSIZE; i++) {
		if (!CPU_ISSET(i, &allowed))
			continue;
		if (cpu_control < 0)
			cpu_control = i;
		else if (cpu_test < 0)
			cpu_test = i;
	}
	if (cpu_test < 0)
		cpu_test = cpu_control;
	ksft_print_msg("controller CPU %d, worker CPU %d, core mode %s\n",
		       cpu_control, cpu_test, core_mode ? "on" : "off");
	if (pin_cpu(cpu_control))
		ksft_exit_fail_msg("pin controller: %s\n", strerror(errno));
	if (set_attr(SCHED_EEVDF, 0, 0, 0))
		ksft_exit_skip("Cannot enter SCHED_EEVDF (CAP_SYS_NICE required): %s\n",
			       strerror(errno));
	alarm(30);
	ksft_set_plan(17);
	ksft_test_result(!get_attr(&attr) && attr.policy == SCHED_EEVDF &&
			 attr.runtime == DEFAULT_SLICE &&
			 sched_get_priority_min(SCHED_EEVDF) == 0,
			 "policy, default slice and priority queries\n");
	ok = !set_attr(SCHED_EEVDF, 0, 200000, 0) && !get_attr(&attr) &&
		attr.runtime == 200000;
	ok = !set_attr(0, 0, 100000, KEEP_POLICY) && !get_attr(&attr) &&
		attr.runtime == 100000 && attr.policy == SCHED_EEVDF && ok;
	ok = !set_attr(0, 0, 0, KEEP_POLICY | KEEP_PARAMS) &&
		!get_attr(&attr) && attr.runtime == 100000 && ok;
	ok = !set_attr(SCHED_EEVDF, 0, 0, 0) && !get_attr(&attr) &&
		attr.runtime == DEFAULT_SLICE && ok;
	ksft_test_result(ok, "slice update, KEEP flags and default reset\n");
	ksft_test_result(invalid_params(), "reject invalid slice, deadline, period and priority\n");
	ok = !setpriority(PRIO_PROCESS, 0, 5) && !get_attr(&attr) && attr.nice == 5;
	ok = !set_attr(SCHED_EEVDF, 0, 200000, 0) && !get_attr(&attr) &&
		attr.nice == 0 && ok;
	ksft_test_result(ok, "nice updates through both interfaces\n");
	ok = !sched_setparam(0, &sp) && !sched_setscheduler(0, SCHED_EEVDF, &sp) &&
		!get_attr(&attr) && attr.runtime == 200000;
	ksft_test_result(ok, "legacy interfaces preserve slice\n");
	ksft_test_result(inheritance(false, false), "fork inherits policy, nice and slice\n");
	ksft_test_result(inheritance(false, true), "exec preserves policy, nice and slice\n");
	ksft_test_result(inheritance(true, false), "RESET_ON_FORK returns child to CFS\n");
	ok = !set_attr(SCHED_EEVDF, 0, 100000, 0);
	sp.sched_priority = 10;
	ok = !sched_setscheduler(0, SCHED_FIFO, &sp) && ok;
	ok = !set_attr(SCHED_EEVDF, 0, 100000, 0) && ok;
	burn_cpu(1000000);
	ok = !set_attr(SCHED_OTHER, 0, 0, 0) && sched_getscheduler(0) == SCHED_OTHER && ok;
	ksft_test_result(ok, "CFS / EEVDF / RT class transitions\n");
	ksft_test_result(affinity_test(), "affinity migration and yield\n");
	ksft_test_result(run_pair(0, false, false, false), "equal weights with different slices share CPU\n");
	ksft_test_result(run_pair(5, false, false, false), "nice weights determine CPU share\n");
	ksft_test_result(run_pair(0, true, false, false), "sleeping and CPU-bound tasks both progress\n");
	ksft_test_result(pi_test(), "PI boost and restoration of EEVDF holder\n");
	ksft_test_result(run_pair(0, false, true, false), "CFS has strict priority over EEVDF\n");
	ksft_test_result(stress_requeue(), "24-task tree, sleep, reweight and class-switch stress\n");
	if (core_mode && cpu_control != cpu_test)
		ksft_test_result(run_pair(0, false, false, true),
				 "different core cookies both receive service\n");
	else
		ksft_test_result_skip("use --core and two SMT siblings for forced-idle test\n");
	alarm(0);
	sched_setaffinity(0, sizeof(allowed), &allowed);
	ksft_finished();
}
