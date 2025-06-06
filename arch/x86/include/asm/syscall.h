/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Access to user system call parameters and results
 *
 * Copyright (C) 2008-2009 Red Hat, Inc.  All rights reserved.
 *
 * See asm-generic/syscall.h for descriptions of what we must do here.
 */

#ifndef _ASM_X86_SYSCALL_H
#define _ASM_X86_SYSCALL_H

#include <uapi/linux/audit.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <asm/thread_info.h>	/* for TS_COMPAT */
#include <asm/unistd.h>

/* This is used purely for kernel/trace/trace_syscalls.c */
typedef long (*sys_call_ptr_t)(const struct pt_regs *);
extern const sys_call_ptr_t sys_call_table[];

/*
 * These may not exist, but still put the prototypes in so we
 * can use IS_ENABLED().
 */
extern long ia32_sys_call(const struct pt_regs *, unsigned int nr);
extern long x32_sys_call(const struct pt_regs *, unsigned int nr);
extern long x64_sys_call(const struct pt_regs *, unsigned int nr);

/*
 * Only the low 32 bits of orig_ax are meaningful, so we return int.
 * This importantly ignores the high bits on 64-bit, so comparisons
 * sign-extend the low 32 bits.
 */
static inline int syscall_get_nr(struct task_struct *task, struct pt_regs *regs)
{
	return regs->orig_ax;
}

static inline void syscall_set_nr(struct task_struct *task,
				  struct pt_regs *regs,
				  int nr)
{
	regs->orig_ax = nr;
}

static inline void syscall_rollback(struct task_struct *task,
				    struct pt_regs *regs)
{
	regs->ax = regs->orig_ax;
}

static inline long syscall_get_error(struct task_struct *task,
				     struct pt_regs *regs)
{
	unsigned long error = regs->ax;
#ifdef CONFIG_IA32_EMULATION
	/*
	 * TS_COMPAT is set for 32-bit syscall entries and then
	 * remains set until we return to user mode.
	 */
	if (task->thread_info.status & (TS_COMPAT|TS_I386_REGS_POKED))
		/*
		 * Sign-extend the value so (int)-EFOO becomes (long)-EFOO
		 * and will match correctly in comparisons.
		 */
		error = (long) (int) error;
#endif
	return IS_ERR_VALUE(error) ? error : 0;
}

static inline long syscall_get_return_value(struct task_struct *task,
					    struct pt_regs *regs)
{
	return regs->ax;
}

static inline void syscall_set_return_value(struct task_struct *task,
					    struct pt_regs *regs,
					    int error, long val)
{
	regs->ax = (long) error ?: val;
}

#ifdef CONFIG_X86_32

static inline void syscall_get_arguments(struct task_struct *task,
					 struct pt_regs *regs,
					 unsigned long *args)
{
	args[0] = regs->bx;
	args[1] = regs->cx;
	args[2] = regs->dx;
	args[3] = regs->si;
	args[4] = regs->di;
	args[5] = regs->bp;
}

static inline void syscall_set_arguments(struct task_struct *task,
					 struct pt_regs *regs,
					 const unsigned long *args)
{
	regs->bx = args[0];
	regs->cx = args[1];
	regs->dx = args[2];
	regs->si = args[3];
	regs->di = args[4];
	regs->bp = args[5];
}

static inline int syscall_get_arch(struct task_struct *task)
{
	return AUDIT_ARCH_I386;
}

#else	 /* CONFIG_X86_64 */

static inline void syscall_get_arguments(struct task_struct *task,
					 struct pt_regs *regs,
					 unsigned long *args)
{
# ifdef CONFIG_IA32_EMULATION
	if (task->thread_info.status & TS_COMPAT) {
		*args++ = regs->bx;
		*args++ = regs->cx;
		*args++ = regs->dx;
		*args++ = regs->si;
		*args++ = regs->di;
		*args   = regs->bp;
	} else
# endif
	{
		*args++ = regs->di;
		*args++ = regs->si;
		*args++ = regs->dx;
		*args++ = regs->r10;
		*args++ = regs->r8;
		*args   = regs->r9;
	}
}

static inline void syscall_set_arguments(struct task_struct *task,
					 struct pt_regs *regs,
					 const unsigned long *args)
{
# ifdef CONFIG_IA32_EMULATION
	if (task->thread_info.status & TS_COMPAT) {
		regs->bx = *args++;
		regs->cx = *args++;
		regs->dx = *args++;
		regs->si = *args++;
		regs->di = *args++;
		regs->bp = *args;
	} else
# endif
	{
		regs->di = *args++;
		regs->si = *args++;
		regs->dx = *args++;
		regs->r10 = *args++;
		regs->r8 = *args++;
		regs->r9 = *args;
	}
}

static inline int syscall_get_arch(struct task_struct *task)
{
	/* x32 tasks should be considered AUDIT_ARCH_X86_64. */
	return (IS_ENABLED(CONFIG_IA32_EMULATION) &&
		task->thread_info.status & TS_COMPAT)
		? AUDIT_ARCH_I386 : AUDIT_ARCH_X86_64;
}

bool do_syscall_64(struct pt_regs *regs, int nr);
void do_int80_emulation(struct pt_regs *regs);

#endif	/* CONFIG_X86_32 */

void do_int80_syscall_32(struct pt_regs *regs);
bool do_fast_syscall_32(struct pt_regs *regs);
bool do_SYSENTER_32(struct pt_regs *regs);

#endif	/* _ASM_X86_SYSCALL_H */
