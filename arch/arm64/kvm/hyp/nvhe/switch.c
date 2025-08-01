// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 */

#include <hyp/switch.h>
#include <hyp/sysreg-sr.h>

#include <linux/arm-smccc.h>
#include <linux/kvm_host.h>
#include <linux/types.h>
#include <linux/jump_label.h>
#include <uapi/linux/psci.h>

#include <kvm/arm_psci.h>

#include <asm/barrier.h>
#include <asm/cpufeature.h>
#include <asm/kprobes.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_emulate.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <asm/fpsimd.h>
#include <asm/debug-monitors.h>
#include <asm/processor.h>

#include <nvhe/mem_protect.h>

/* Non-VHE specific context */
DEFINE_PER_CPU(struct kvm_host_data, kvm_host_data);
DEFINE_PER_CPU(struct kvm_cpu_context, kvm_hyp_ctxt);
DEFINE_PER_CPU(unsigned long, kvm_hyp_vector);

struct fgt_masks hfgrtr_masks;
struct fgt_masks hfgwtr_masks;
struct fgt_masks hfgitr_masks;
struct fgt_masks hdfgrtr_masks;
struct fgt_masks hdfgwtr_masks;
struct fgt_masks hafgrtr_masks;
struct fgt_masks hfgrtr2_masks;
struct fgt_masks hfgwtr2_masks;
struct fgt_masks hfgitr2_masks;
struct fgt_masks hdfgrtr2_masks;
struct fgt_masks hdfgwtr2_masks;

extern void kvm_nvhe_prepare_backtrace(unsigned long fp, unsigned long pc);

static void __activate_traps(struct kvm_vcpu *vcpu)
{
	___activate_traps(vcpu, vcpu->arch.hcr_el2);
	__activate_traps_common(vcpu);
	__activate_cptr_traps(vcpu);

	write_sysreg(__this_cpu_read(kvm_hyp_vector), vbar_el2);

	if (cpus_have_final_cap(ARM64_WORKAROUND_SPECULATIVE_AT)) {
		struct kvm_cpu_context *ctxt = &vcpu->arch.ctxt;

		isb();
		/*
		 * At this stage, and thanks to the above isb(), S2 is
		 * configured and enabled. We can now restore the guest's S1
		 * configuration: SCTLR, and only then TCR.
		 */
		write_sysreg_el1(ctxt_sys_reg(ctxt, SCTLR_EL1),	SYS_SCTLR);
		isb();
		write_sysreg_el1(ctxt_sys_reg(ctxt, TCR_EL1),	SYS_TCR);
	}
}

static void __deactivate_traps(struct kvm_vcpu *vcpu)
{
	extern char __kvm_hyp_host_vector[];

	___deactivate_traps(vcpu);

	if (cpus_have_final_cap(ARM64_WORKAROUND_SPECULATIVE_AT)) {
		u64 val;

		/*
		 * Set the TCR and SCTLR registers in the exact opposite
		 * sequence as __activate_traps (first prevent walks,
		 * then force the MMU on). A generous sprinkling of isb()
		 * ensure that things happen in this exact order.
		 */
		val = read_sysreg_el1(SYS_TCR);
		write_sysreg_el1(val | TCR_EPD1_MASK | TCR_EPD0_MASK, SYS_TCR);
		isb();
		val = read_sysreg_el1(SYS_SCTLR);
		write_sysreg_el1(val | SCTLR_ELx_M, SYS_SCTLR);
		isb();
	}

	__deactivate_traps_common(vcpu);

	write_sysreg_hcr(this_cpu_ptr(&kvm_init_params)->hcr_el2);

	__deactivate_cptr_traps(vcpu);
	write_sysreg(__kvm_hyp_host_vector, vbar_el2);
}

/* Save VGICv3 state on non-VHE systems */
static void __hyp_vgic_save_state(struct kvm_vcpu *vcpu)
{
	if (static_branch_unlikely(&kvm_vgic_global_state.gicv3_cpuif)) {
		__vgic_v3_save_state(&vcpu->arch.vgic_cpu.vgic_v3);
		__vgic_v3_deactivate_traps(&vcpu->arch.vgic_cpu.vgic_v3);
	}
}

/* Restore VGICv3 state on non-VHE systems */
static void __hyp_vgic_restore_state(struct kvm_vcpu *vcpu)
{
	if (static_branch_unlikely(&kvm_vgic_global_state.gicv3_cpuif)) {
		__vgic_v3_activate_traps(&vcpu->arch.vgic_cpu.vgic_v3);
		__vgic_v3_restore_state(&vcpu->arch.vgic_cpu.vgic_v3);
	}
}

/*
 * Disable host events, enable guest events
 */
#ifdef CONFIG_HW_PERF_EVENTS
static bool __pmu_switch_to_guest(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu_events *pmu = &vcpu->arch.pmu.events;

	if (pmu->events_host)
		write_sysreg(pmu->events_host, pmcntenclr_el0);

	if (pmu->events_guest)
		write_sysreg(pmu->events_guest, pmcntenset_el0);

	return (pmu->events_host || pmu->events_guest);
}

/*
 * Disable guest events, enable host events
 */
static void __pmu_switch_to_host(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu_events *pmu = &vcpu->arch.pmu.events;

	if (pmu->events_guest)
		write_sysreg(pmu->events_guest, pmcntenclr_el0);

	if (pmu->events_host)
		write_sysreg(pmu->events_host, pmcntenset_el0);
}
#else
#define __pmu_switch_to_guest(v)	({ false; })
#define __pmu_switch_to_host(v)		do {} while (0)
#endif

/*
 * Handler for protected VM MSR, MRS or System instruction execution in AArch64.
 *
 * Returns true if the hypervisor has handled the exit, and control should go
 * back to the guest, or false if it hasn't.
 */
static bool kvm_handle_pvm_sys64(struct kvm_vcpu *vcpu, u64 *exit_code)
{
	/*
	 * Make sure we handle the exit for workarounds before the pKVM
	 * handling, as the latter could decide to UNDEF.
	 */
	return (kvm_hyp_handle_sysreg(vcpu, exit_code) ||
		kvm_handle_pvm_sysreg(vcpu, exit_code));
}

static const exit_handler_fn hyp_exit_handlers[] = {
	[0 ... ESR_ELx_EC_MAX]		= NULL,
	[ESR_ELx_EC_CP15_32]		= kvm_hyp_handle_cp15_32,
	[ESR_ELx_EC_SYS64]		= kvm_hyp_handle_sysreg,
	[ESR_ELx_EC_SVE]		= kvm_hyp_handle_fpsimd,
	[ESR_ELx_EC_FP_ASIMD]		= kvm_hyp_handle_fpsimd,
	[ESR_ELx_EC_IABT_LOW]		= kvm_hyp_handle_iabt_low,
	[ESR_ELx_EC_DABT_LOW]		= kvm_hyp_handle_dabt_low,
	[ESR_ELx_EC_WATCHPT_LOW]	= kvm_hyp_handle_watchpt_low,
	[ESR_ELx_EC_MOPS]		= kvm_hyp_handle_mops,
};

static const exit_handler_fn pvm_exit_handlers[] = {
	[0 ... ESR_ELx_EC_MAX]		= NULL,
	[ESR_ELx_EC_SYS64]		= kvm_handle_pvm_sys64,
	[ESR_ELx_EC_SVE]		= kvm_handle_pvm_restricted,
	[ESR_ELx_EC_FP_ASIMD]		= kvm_hyp_handle_fpsimd,
	[ESR_ELx_EC_IABT_LOW]		= kvm_hyp_handle_iabt_low,
	[ESR_ELx_EC_DABT_LOW]		= kvm_hyp_handle_dabt_low,
	[ESR_ELx_EC_WATCHPT_LOW]	= kvm_hyp_handle_watchpt_low,
	[ESR_ELx_EC_MOPS]		= kvm_hyp_handle_mops,
};

static const exit_handler_fn *kvm_get_exit_handler_array(struct kvm_vcpu *vcpu)
{
	if (unlikely(vcpu_is_protected(vcpu)))
		return pvm_exit_handlers;

	return hyp_exit_handlers;
}

static inline bool fixup_guest_exit(struct kvm_vcpu *vcpu, u64 *exit_code)
{
	const exit_handler_fn *handlers = kvm_get_exit_handler_array(vcpu);

	synchronize_vcpu_pstate(vcpu, exit_code);

	/*
	 * Some guests (e.g., protected VMs) are not be allowed to run in
	 * AArch32.  The ARMv8 architecture does not give the hypervisor a
	 * mechanism to prevent a guest from dropping to AArch32 EL0 if
	 * implemented by the CPU. If the hypervisor spots a guest in such a
	 * state ensure it is handled, and don't trust the host to spot or fix
	 * it.  The check below is based on the one in
	 * kvm_arch_vcpu_ioctl_run().
	 */
	if (unlikely(vcpu_is_protected(vcpu) && vcpu_mode_is_32bit(vcpu))) {
		/*
		 * As we have caught the guest red-handed, decide that it isn't
		 * fit for purpose anymore by making the vcpu invalid. The VMM
		 * can try and fix it by re-initializing the vcpu with
		 * KVM_ARM_VCPU_INIT, however, this is likely not possible for
		 * protected VMs.
		 */
		vcpu_clear_flag(vcpu, VCPU_INITIALIZED);
		*exit_code &= BIT(ARM_EXIT_WITH_SERROR_BIT);
		*exit_code |= ARM_EXCEPTION_IL;
	}

	return __fixup_guest_exit(vcpu, exit_code, handlers);
}

/* Switch to the guest for legacy non-VHE systems */
int __kvm_vcpu_run(struct kvm_vcpu *vcpu)
{
	struct kvm_cpu_context *host_ctxt;
	struct kvm_cpu_context *guest_ctxt;
	struct kvm_s2_mmu *mmu;
	bool pmu_switch_needed;
	u64 exit_code;

	/*
	 * Having IRQs masked via PMR when entering the guest means the GIC
	 * will not signal the CPU of interrupts of lower priority, and the
	 * only way to get out will be via guest exceptions.
	 * Naturally, we want to avoid this.
	 */
	if (system_uses_irq_prio_masking()) {
		gic_write_pmr(GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET);
		pmr_sync();
	}

	host_ctxt = host_data_ptr(host_ctxt);
	host_ctxt->__hyp_running_vcpu = vcpu;
	guest_ctxt = &vcpu->arch.ctxt;

	pmu_switch_needed = __pmu_switch_to_guest(vcpu);

	__sysreg_save_state_nvhe(host_ctxt);
	/*
	 * We must flush and disable the SPE buffer for nVHE, as
	 * the translation regime(EL1&0) is going to be loaded with
	 * that of the guest. And we must do this before we change the
	 * translation regime to EL2 (via MDCR_EL2_E2PB == 0) and
	 * before we load guest Stage1.
	 */
	__debug_save_host_buffers_nvhe(vcpu);

	/*
	 * We're about to restore some new MMU state. Make sure
	 * ongoing page-table walks that have started before we
	 * trapped to EL2 have completed. This also synchronises the
	 * above disabling of BRBE, SPE and TRBE.
	 *
	 * See DDI0487I.a D8.1.5 "Out-of-context translation regimes",
	 * rule R_LFHQG and subsequent information statements.
	 */
	dsb(nsh);

	__kvm_adjust_pc(vcpu);

	/*
	 * We must restore the 32-bit state before the sysregs, thanks
	 * to erratum #852523 (Cortex-A57) or #853709 (Cortex-A72).
	 *
	 * Also, and in order to be able to deal with erratum #1319537 (A57)
	 * and #1319367 (A72), we must ensure that all VM-related sysreg are
	 * restored before we enable S2 translation.
	 */
	__sysreg32_restore_state(vcpu);
	__sysreg_restore_state_nvhe(guest_ctxt);

	mmu = kern_hyp_va(vcpu->arch.hw_mmu);
	__load_stage2(mmu, kern_hyp_va(mmu->arch));
	__activate_traps(vcpu);

	__hyp_vgic_restore_state(vcpu);
	__timer_enable_traps(vcpu);

	__debug_switch_to_guest(vcpu);

	do {
		/* Jump in the fire! */
		exit_code = __guest_enter(vcpu);

		/* And we're baaack! */
	} while (fixup_guest_exit(vcpu, &exit_code));

	__sysreg_save_state_nvhe(guest_ctxt);
	__sysreg32_save_state(vcpu);
	__timer_disable_traps(vcpu);
	__hyp_vgic_save_state(vcpu);

	/*
	 * Same thing as before the guest run: we're about to switch
	 * the MMU context, so let's make sure we don't have any
	 * ongoing EL1&0 translations.
	 */
	dsb(nsh);

	__deactivate_traps(vcpu);
	__load_host_stage2();

	__sysreg_restore_state_nvhe(host_ctxt);

	if (guest_owns_fp_regs())
		__fpsimd_save_fpexc32(vcpu);

	__debug_switch_to_host(vcpu);
	/*
	 * This must come after restoring the host sysregs, since a non-VHE
	 * system may enable SPE here and make use of the TTBRs.
	 */
	__debug_restore_host_buffers_nvhe(vcpu);

	if (pmu_switch_needed)
		__pmu_switch_to_host(vcpu);

	/* Returning to host will clear PSR.I, remask PMR if needed */
	if (system_uses_irq_prio_masking())
		gic_write_pmr(GIC_PRIO_IRQOFF);

	host_ctxt->__hyp_running_vcpu = NULL;

	return exit_code;
}

asmlinkage void __noreturn hyp_panic(void)
{
	u64 spsr = read_sysreg_el2(SYS_SPSR);
	u64 elr = read_sysreg_el2(SYS_ELR);
	u64 par = read_sysreg_par();
	struct kvm_cpu_context *host_ctxt;
	struct kvm_vcpu *vcpu;

	host_ctxt = host_data_ptr(host_ctxt);
	vcpu = host_ctxt->__hyp_running_vcpu;

	if (vcpu) {
		__timer_disable_traps(vcpu);
		__deactivate_traps(vcpu);
		__load_host_stage2();
		__sysreg_restore_state_nvhe(host_ctxt);
	}

	/* Prepare to dump kvm nvhe hyp stacktrace */
	kvm_nvhe_prepare_backtrace((unsigned long)__builtin_frame_address(0),
				   _THIS_IP_);

	__hyp_do_panic(host_ctxt, spsr, elr, par);
	unreachable();
}

asmlinkage void __noreturn hyp_panic_bad_stack(void)
{
	hyp_panic();
}

asmlinkage void kvm_unexpected_el2_exception(void)
{
	__kvm_unexpected_el2_exception();
}
