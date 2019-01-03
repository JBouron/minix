/* Implementation of CPU local variables generics */
#ifndef __CPULOCALS_H__
#define __CPULOCALS_H__

#ifndef __ASSEMBLY__

#ifdef CONFIG_SMP

/* SMP */

#define CPULOCAL_ARRAY	[CONFIG_MAX_CPUS]

#define get_cpu_var(cpu, name)		__cpu_local_vars[cpu].name
#define get_cpu_var_ptr(cpu, name)	(&(get_cpu_var(cpu, name)))
#define get_cpulocal_var(name)		get_cpu_var(cpuid, name)
#define get_cpulocal_var_ptr(name)	get_cpu_var_ptr(cpuid, name)

#define lock_runqueues(cpu) _reentrantlock_lock(&(__cpu_local_vars[cpu].q_lock))
#define unlock_runqueues(cpu) _reentrantlock_unlock(&(__cpu_local_vars[cpu].q_lock))

/* FIXME - padd the structure so that items in the array do not share cacheline
 * with other cpus */

#else

/* single CPU */

#define CPULOCAL_ARRAY

#define get_cpulocal_var(name)		__cpu_local_vars.name
#define get_cpulocal_var_ptr(name)	&(get_cpulocal_var(name))
#define get_cpu_var(cpu, name)		get_cpulocal_var(name)
#define get_cpu_var_ptr(cpu, name)	get_cpulocal_var_ptr(name)

#define lock_runqueues(cpu)
#define unlock_runqueues(cpu)
#endif

/*
 * The global cpu local variables in use
 */
extern struct __cpu_local_vars {

/* Process scheduling information and the kernel reentry count. */
	struct proc *proc_ptr;/* pointer to currently running process */
	struct proc *bill_ptr;/* process to bill for clock ticks */
	struct proc idle_proc;/* stub for an idle process */

	struct proc *bill_ipc; /* process to bill for ipc. */
	struct proc *bill_kcall; /* process to bill for kernel call. */

	reentrantlock_t q_lock;	/* Run queues lock. Also protects the idle state. */

	int n_receive;
	int n_receive_any;

	int n_retries_recv;
	int n_retries_recv_all_null;

	int catch_pagefaults;

/* 
 * signal whether pagefault is already being handled to detect recursive
 * pagefaults
 */
	int pagefault_handled;

/*
 * which processpage tables are loaded right now. We need to know this because
 * some processes are loaded in each process pagetables and don't have their own
 * pagetables. Therefore we cannot use the proc_ptr pointer
 */
	struct proc * ptproc;

/* CPU private run queues */
	struct proc * run_q_head[NR_SCHED_QUEUES]; /* ptrs to ready list headers */
	struct proc * run_q_tail[NR_SCHED_QUEUES]; /* ptrs to ready list tails */
	int cpu_is_idle; /* let the others know that you are idle */

	int idle_interrupted; /* to interrupt busy-idle
						     while profiling */

	u64_t tsc_ctr_switch; /* when did we switched time accounting */

/* last values read from cpu when sending ooq msg to scheduler */
	u64_t cpu_last_tsc;
	u64_t cpu_last_idle;


	char fpu_presence; /* whether the cpu has FPU or not */
	struct proc * fpu_owner; /* who owns the FPU of the local cpu */

} __cpu_local_vars CPULOCAL_ARRAY;

#endif /* __ASSEMBLY__ */

#endif /* __CPULOCALS_H__ */
