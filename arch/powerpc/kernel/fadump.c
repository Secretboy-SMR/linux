// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Firmware Assisted dump: A robust mechanism to get reliable kernel crash
 * dump with assistance from firmware. This approach does not use kexec,
 * instead firmware assists in booting the kdump kernel while preserving
 * memory contents. The most of the code implementation has been adapted
 * from phyp assisted dump implementation written by Linas Vepstas and
 * Manish Ahuja
 *
 * Copyright 2011 IBM Corporation
 * Author: Mahesh Salgaonkar <mahesh@linux.vnet.ibm.com>
 */

#undef DEBUG
#define pr_fmt(fmt) "fadump: " fmt

#include <linux/string.h>
#include <linux/memblock.h>
#include <linux/delay.h>
#include <linux/seq_file.h>
#include <linux/crash_dump.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/cma.h>
#include <linux/hugetlb.h>
#include <linux/debugfs.h>
#include <linux/of.h>
#include <linux/of_fdt.h>

#include <asm/page.h>
#include <asm/fadump.h>
#include <asm/fadump-internal.h>
#include <asm/setup.h>
#include <asm/interrupt.h>
#include <asm/prom.h>

/*
 * The CPU who acquired the lock to trigger the fadump crash should
 * wait for other CPUs to enter.
 *
 * The timeout is in milliseconds.
 */
#define CRASH_TIMEOUT		500

static struct fw_dump fw_dump;

static void __init fadump_reserve_crash_area(u64 base);

#ifndef CONFIG_PRESERVE_FA_DUMP

static struct kobject *fadump_kobj;

static atomic_t cpus_in_fadump;
static DEFINE_MUTEX(fadump_mutex);

#define RESERVED_RNGS_SZ	16384 /* 16K - 128 entries */
#define RESERVED_RNGS_CNT	(RESERVED_RNGS_SZ / \
				 sizeof(struct fadump_memory_range))
static struct fadump_memory_range rngs[RESERVED_RNGS_CNT];
static struct fadump_mrange_info
reserved_mrange_info = { "reserved", rngs, RESERVED_RNGS_SZ, 0, RESERVED_RNGS_CNT, true };

static void __init early_init_dt_scan_reserved_ranges(unsigned long node);

#ifdef CONFIG_CMA
static struct cma *fadump_cma;

/*
 * fadump_cma_init() - Initialize CMA area from a fadump reserved memory
 *
 * This function initializes CMA area from fadump reserved memory.
 * The total size of fadump reserved memory covers for boot memory size
 * + cpu data size + hpte size and metadata.
 * Initialize only the area equivalent to boot memory size for CMA use.
 * The remaining portion of fadump reserved memory will be not given
 * to CMA and pages for those will stay reserved. boot memory size is
 * aligned per CMA requirement to satisy cma_init_reserved_mem() call.
 * But for some reason even if it fails we still have the memory reservation
 * with us and we can still continue doing fadump.
 */
void __init fadump_cma_init(void)
{
	unsigned long long base, size, end;
	int rc;

	if (!fw_dump.fadump_supported || !fw_dump.fadump_enabled ||
			fw_dump.dump_active)
		return;
	/*
	 * Do not use CMA if user has provided fadump=nocma kernel parameter.
	 */
	if (fw_dump.nocma || !fw_dump.boot_memory_size)
		return;

	/*
	 * [base, end) should be reserved during early init in
	 * fadump_reserve_mem(). No need to check this here as
	 * cma_init_reserved_mem() already checks for overlap.
	 * Here we give the aligned chunk of this reserved memory to CMA.
	 */
	base = fw_dump.reserve_dump_area_start;
	size = fw_dump.boot_memory_size;
	end = base + size;

	base = ALIGN(base, CMA_MIN_ALIGNMENT_BYTES);
	end = ALIGN_DOWN(end, CMA_MIN_ALIGNMENT_BYTES);
	size = end - base;

	if (end <= base) {
		pr_warn("%s: Too less memory to give to CMA\n", __func__);
		return;
	}

	rc = cma_init_reserved_mem(base, size, 0, "fadump_cma", &fadump_cma);
	if (rc) {
		pr_err("Failed to init cma area for firmware-assisted dump,%d\n", rc);
		/*
		 * Though the CMA init has failed we still have memory
		 * reservation with us. The reserved memory will be
		 * blocked from production system usage.  Hence return 1,
		 * so that we can continue with fadump.
		 */
		return;
	}

	/*
	 *  If CMA activation fails, keep the pages reserved, instead of
	 *  exposing them to buddy allocator. Same as 'fadump=nocma' case.
	 */
	cma_reserve_pages_on_error(fadump_cma);

	/*
	 * So we now have successfully initialized cma area for fadump.
	 */
	pr_info("Initialized [0x%llx, %luMB] cma area from [0x%lx, %luMB] "
		"bytes of memory reserved for firmware-assisted dump\n",
		cma_get_base(fadump_cma), cma_get_size(fadump_cma) >> 20,
		fw_dump.reserve_dump_area_start,
		fw_dump.boot_memory_size >> 20);
	return;
}
#endif /* CONFIG_CMA */

/*
 * Additional parameters meant for capture kernel are placed in a dedicated area.
 * If this is capture kernel boot, append these parameters to bootargs.
 */
void __init fadump_append_bootargs(void)
{
	char *append_args;
	size_t len;

	if (!fw_dump.dump_active || !fw_dump.param_area_supported || !fw_dump.param_area)
		return;

	if (fw_dump.param_area < fw_dump.boot_mem_top) {
		if (memblock_reserve(fw_dump.param_area, COMMAND_LINE_SIZE)) {
			pr_warn("WARNING: Can't use additional parameters area!\n");
			fw_dump.param_area = 0;
			return;
		}
	}

	append_args = (char *)fw_dump.param_area;
	len = strlen(boot_command_line);

	/*
	 * Too late to fail even if cmdline size exceeds. Truncate additional parameters
	 * to cmdline size and proceed anyway.
	 */
	if (len + strlen(append_args) >= COMMAND_LINE_SIZE - 1)
		pr_warn("WARNING: Appending parameters exceeds cmdline size. Truncating!\n");

	pr_debug("Cmdline: %s\n", boot_command_line);
	snprintf(boot_command_line + len, COMMAND_LINE_SIZE - len, " %s", append_args);
	pr_info("Updated cmdline: %s\n", boot_command_line);
}

/* Scan the Firmware Assisted dump configuration details. */
int __init early_init_dt_scan_fw_dump(unsigned long node, const char *uname,
				      int depth, void *data)
{
	if (depth == 0) {
		early_init_dt_scan_reserved_ranges(node);
		return 0;
	}

	if (depth != 1)
		return 0;

	if (strcmp(uname, "rtas") == 0) {
		rtas_fadump_dt_scan(&fw_dump, node);
		return 1;
	}

	if (strcmp(uname, "ibm,opal") == 0) {
		opal_fadump_dt_scan(&fw_dump, node);
		return 1;
	}

	return 0;
}

/*
 * If fadump is registered, check if the memory provided
 * falls within boot memory area and reserved memory area.
 */
int is_fadump_memory_area(u64 addr, unsigned long size)
{
	u64 d_start, d_end;

	if (!fw_dump.dump_registered)
		return 0;

	if (!size)
		return 0;

	d_start = fw_dump.reserve_dump_area_start;
	d_end = d_start + fw_dump.reserve_dump_area_size;
	if (((addr + size) > d_start) && (addr <= d_end))
		return 1;

	return (addr <= fw_dump.boot_mem_top);
}

int should_fadump_crash(void)
{
	if (!fw_dump.dump_registered || !fw_dump.fadumphdr_addr)
		return 0;
	return 1;
}

int is_fadump_active(void)
{
	return fw_dump.dump_active;
}

/*
 * Returns true, if there are no holes in memory area between d_start to d_end,
 * false otherwise.
 */
static bool is_fadump_mem_area_contiguous(u64 d_start, u64 d_end)
{
	phys_addr_t reg_start, reg_end;
	bool ret = false;
	u64 i, start, end;

	for_each_mem_range(i, &reg_start, &reg_end) {
		start = max_t(u64, d_start, reg_start);
		end = min_t(u64, d_end, reg_end);
		if (d_start < end) {
			/* Memory hole from d_start to start */
			if (start > d_start)
				break;

			if (end == d_end) {
				ret = true;
				break;
			}

			d_start = end + 1;
		}
	}

	return ret;
}

/*
 * Returns true, if there are no holes in reserved memory area,
 * false otherwise.
 */
bool is_fadump_reserved_mem_contiguous(void)
{
	u64 d_start, d_end;

	d_start	= fw_dump.reserve_dump_area_start;
	d_end	= d_start + fw_dump.reserve_dump_area_size;
	return is_fadump_mem_area_contiguous(d_start, d_end);
}

/* Print firmware assisted dump configurations for debugging purpose. */
static void __init fadump_show_config(void)
{
	int i;

	pr_debug("Support for firmware-assisted dump (fadump): %s\n",
			(fw_dump.fadump_supported ? "present" : "no support"));

	if (!fw_dump.fadump_supported)
		return;

	pr_debug("Fadump enabled    : %s\n", str_yes_no(fw_dump.fadump_enabled));
	pr_debug("Dump Active       : %s\n", str_yes_no(fw_dump.dump_active));
	pr_debug("Dump section sizes:\n");
	pr_debug("    CPU state data size: %lx\n", fw_dump.cpu_state_data_size);
	pr_debug("    HPTE region size   : %lx\n", fw_dump.hpte_region_size);
	pr_debug("    Boot memory size   : %lx\n", fw_dump.boot_memory_size);
	pr_debug("    Boot memory top    : %llx\n", fw_dump.boot_mem_top);
	pr_debug("Boot memory regions cnt: %llx\n", fw_dump.boot_mem_regs_cnt);
	for (i = 0; i < fw_dump.boot_mem_regs_cnt; i++) {
		pr_debug("[%03d] base = %llx, size = %llx\n", i,
			 fw_dump.boot_mem_addr[i], fw_dump.boot_mem_sz[i]);
	}
}

/**
 * fadump_calculate_reserve_size(): reserve variable boot area 5% of System RAM
 *
 * Function to find the largest memory size we need to reserve during early
 * boot process. This will be the size of the memory that is required for a
 * kernel to boot successfully.
 *
 * This function has been taken from phyp-assisted dump feature implementation.
 *
 * returns larger of 256MB or 5% rounded down to multiples of 256MB.
 *
 * TODO: Come up with better approach to find out more accurate memory size
 * that is required for a kernel to boot successfully.
 *
 */
static __init u64 fadump_calculate_reserve_size(void)
{
	u64 base, size, bootmem_min;
	int ret;

	if (fw_dump.reserve_bootvar)
		pr_warn("'fadump_reserve_mem=' parameter is deprecated in favor of 'crashkernel=' parameter.\n");

	/*
	 * Check if the size is specified through crashkernel= cmdline
	 * option. If yes, then use that but ignore base as fadump reserves
	 * memory at a predefined offset.
	 */
	ret = parse_crashkernel(boot_command_line, memblock_phys_mem_size(),
				&size, &base, NULL, NULL);
	if (ret == 0 && size > 0) {
		unsigned long max_size;

		if (fw_dump.reserve_bootvar)
			pr_info("Using 'crashkernel=' parameter for memory reservation.\n");

		fw_dump.reserve_bootvar = (unsigned long)size;

		/*
		 * Adjust if the boot memory size specified is above
		 * the upper limit.
		 */
		max_size = memblock_phys_mem_size() / MAX_BOOT_MEM_RATIO;
		if (fw_dump.reserve_bootvar > max_size) {
			fw_dump.reserve_bootvar = max_size;
			pr_info("Adjusted boot memory size to %luMB\n",
				(fw_dump.reserve_bootvar >> 20));
		}

		return fw_dump.reserve_bootvar;
	} else if (fw_dump.reserve_bootvar) {
		/*
		 * 'fadump_reserve_mem=' is being used to reserve memory
		 * for firmware-assisted dump.
		 */
		return fw_dump.reserve_bootvar;
	}

	/* divide by 20 to get 5% of value */
	size = memblock_phys_mem_size() / 20;

	/* round it down in multiples of 256 */
	size = size & ~0x0FFFFFFFUL;

	/* Truncate to memory_limit. We don't want to over reserve the memory.*/
	if (memory_limit && size > memory_limit)
		size = memory_limit;

	bootmem_min = fw_dump.ops->fadump_get_bootmem_min();
	return (size > bootmem_min ? size : bootmem_min);
}

/*
 * Calculate the total memory size required to be reserved for
 * firmware-assisted dump registration.
 */
static unsigned long __init get_fadump_area_size(void)
{
	unsigned long size = 0;

	size += fw_dump.cpu_state_data_size;
	size += fw_dump.hpte_region_size;
	/*
	 * Account for pagesize alignment of boot memory area destination address.
	 * This faciliates in mmap reading of first kernel's memory.
	 */
	size = PAGE_ALIGN(size);
	size += fw_dump.boot_memory_size;
	size += sizeof(struct fadump_crash_info_header);

	/* This is to hold kernel metadata on platforms that support it */
	size += (fw_dump.ops->fadump_get_metadata_size ?
		 fw_dump.ops->fadump_get_metadata_size() : 0);
	return size;
}

static int __init add_boot_mem_region(unsigned long rstart,
				      unsigned long rsize)
{
	int max_boot_mem_rgns = fw_dump.ops->fadump_max_boot_mem_rgns();
	int i = fw_dump.boot_mem_regs_cnt++;

	if (fw_dump.boot_mem_regs_cnt > max_boot_mem_rgns) {
		fw_dump.boot_mem_regs_cnt = max_boot_mem_rgns;
		return 0;
	}

	pr_debug("Added boot memory range[%d] [%#016lx-%#016lx)\n",
		 i, rstart, (rstart + rsize));
	fw_dump.boot_mem_addr[i] = rstart;
	fw_dump.boot_mem_sz[i] = rsize;
	return 1;
}

/*
 * Firmware usually has a hard limit on the data it can copy per region.
 * Honour that by splitting a memory range into multiple regions.
 */
static int __init add_boot_mem_regions(unsigned long mstart,
				       unsigned long msize)
{
	unsigned long rstart, rsize, max_size;
	int ret = 1;

	rstart = mstart;
	max_size = fw_dump.max_copy_size ? fw_dump.max_copy_size : msize;
	while (msize) {
		if (msize > max_size)
			rsize = max_size;
		else
			rsize = msize;

		ret = add_boot_mem_region(rstart, rsize);
		if (!ret)
			break;

		msize -= rsize;
		rstart += rsize;
	}

	return ret;
}

static int __init fadump_get_boot_mem_regions(void)
{
	unsigned long size, cur_size, hole_size, last_end;
	unsigned long mem_size = fw_dump.boot_memory_size;
	phys_addr_t reg_start, reg_end;
	int ret = 1;
	u64 i;

	fw_dump.boot_mem_regs_cnt = 0;

	last_end = 0;
	hole_size = 0;
	cur_size = 0;
	for_each_mem_range(i, &reg_start, &reg_end) {
		size = reg_end - reg_start;
		hole_size += (reg_start - last_end);

		if ((cur_size + size) >= mem_size) {
			size = (mem_size - cur_size);
			ret = add_boot_mem_regions(reg_start, size);
			break;
		}

		mem_size -= size;
		cur_size += size;
		ret = add_boot_mem_regions(reg_start, size);
		if (!ret)
			break;

		last_end = reg_end;
	}
	fw_dump.boot_mem_top = PAGE_ALIGN(fw_dump.boot_memory_size + hole_size);

	return ret;
}

/*
 * Returns true, if the given range overlaps with reserved memory ranges
 * starting at idx. Also, updates idx to index of overlapping memory range
 * with the given memory range.
 * False, otherwise.
 */
static bool __init overlaps_reserved_ranges(u64 base, u64 end, int *idx)
{
	bool ret = false;
	int i;

	for (i = *idx; i < reserved_mrange_info.mem_range_cnt; i++) {
		u64 rbase = reserved_mrange_info.mem_ranges[i].base;
		u64 rend = rbase + reserved_mrange_info.mem_ranges[i].size;

		if (end <= rbase)
			break;

		if ((end > rbase) &&  (base < rend)) {
			*idx = i;
			ret = true;
			break;
		}
	}

	return ret;
}

/*
 * Locate a suitable memory area to reserve memory for FADump. While at it,
 * lookup reserved-ranges & avoid overlap with them, as they are used by F/W.
 */
static u64 __init fadump_locate_reserve_mem(u64 base, u64 size)
{
	struct fadump_memory_range *mrngs;
	phys_addr_t mstart, mend;
	int idx = 0;
	u64 i, ret = 0;

	mrngs = reserved_mrange_info.mem_ranges;
	for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE,
				&mstart, &mend, NULL) {
		pr_debug("%llu) mstart: %llx, mend: %llx, base: %llx\n",
			 i, mstart, mend, base);

		if (mstart > base)
			base = PAGE_ALIGN(mstart);

		while ((mend > base) && ((mend - base) >= size)) {
			if (!overlaps_reserved_ranges(base, base+size, &idx)) {
				ret = base;
				goto out;
			}

			base = mrngs[idx].base + mrngs[idx].size;
			base = PAGE_ALIGN(base);
		}
	}

out:
	return ret;
}

int __init fadump_reserve_mem(void)
{
	u64 base, size, mem_boundary, bootmem_min;
	int ret = 1;

	if (!fw_dump.fadump_enabled)
		return 0;

	if (!fw_dump.fadump_supported) {
		pr_info("Firmware-Assisted Dump is not supported on this hardware\n");
		goto error_out;
	}

	/*
	 * Initialize boot memory size
	 * If dump is active then we have already calculated the size during
	 * first kernel.
	 */
	if (!fw_dump.dump_active) {
		fw_dump.boot_memory_size =
			PAGE_ALIGN(fadump_calculate_reserve_size());

		bootmem_min = fw_dump.ops->fadump_get_bootmem_min();
		if (fw_dump.boot_memory_size < bootmem_min) {
			pr_err("Can't enable fadump with boot memory size (0x%lx) less than 0x%llx\n",
			       fw_dump.boot_memory_size, bootmem_min);
			goto error_out;
		}

		if (!fadump_get_boot_mem_regions()) {
			pr_err("Too many holes in boot memory area to enable fadump\n");
			goto error_out;
		}
	}

	if (memory_limit)
		mem_boundary = memory_limit;
	else
		mem_boundary = memblock_end_of_DRAM();

	base = fw_dump.boot_mem_top;
	size = get_fadump_area_size();
	fw_dump.reserve_dump_area_size = size;
	if (fw_dump.dump_active) {
		pr_info("Firmware-assisted dump is active.\n");

#ifdef CONFIG_HUGETLB_PAGE
		/*
		 * FADump capture kernel doesn't care much about hugepages.
		 * In fact, handling hugepages in capture kernel is asking for
		 * trouble. So, disable HugeTLB support when fadump is active.
		 */
		hugetlb_disabled = true;
#endif
		/*
		 * If last boot has crashed then reserve all the memory
		 * above boot memory size so that we don't touch it until
		 * dump is written to disk by userspace tool. This memory
		 * can be released for general use by invalidating fadump.
		 */
		fadump_reserve_crash_area(base);

		pr_debug("fadumphdr_addr = %#016lx\n", fw_dump.fadumphdr_addr);
		pr_debug("Reserve dump area start address: 0x%lx\n",
			 fw_dump.reserve_dump_area_start);
	} else {
		/*
		 * Reserve memory at an offset closer to bottom of the RAM to
		 * minimize the impact of memory hot-remove operation.
		 */
		base = fadump_locate_reserve_mem(base, size);

		if (!base || (base + size > mem_boundary)) {
			pr_err("Failed to find memory chunk for reservation!\n");
			goto error_out;
		}
		fw_dump.reserve_dump_area_start = base;

		/*
		 * Calculate the kernel metadata address and register it with
		 * f/w if the platform supports.
		 */
		if (fw_dump.ops->fadump_setup_metadata &&
		    (fw_dump.ops->fadump_setup_metadata(&fw_dump) < 0))
			goto error_out;

		if (memblock_reserve(base, size)) {
			pr_err("Failed to reserve memory!\n");
			goto error_out;
		}

		pr_info("Reserved %lldMB of memory at %#016llx (System RAM: %lldMB)\n",
			(size >> 20), base, (memblock_phys_mem_size() >> 20));
	}

	return ret;
error_out:
	fw_dump.fadump_enabled = 0;
	fw_dump.reserve_dump_area_size = 0;
	return 0;
}

/* Look for fadump= cmdline option. */
static int __init early_fadump_param(char *p)
{
	if (!p)
		return 1;

	if (strncmp(p, "on", 2) == 0)
		fw_dump.fadump_enabled = 1;
	else if (strncmp(p, "off", 3) == 0)
		fw_dump.fadump_enabled = 0;
	else if (strncmp(p, "nocma", 5) == 0) {
		fw_dump.fadump_enabled = 1;
		fw_dump.nocma = 1;
	}

	return 0;
}
early_param("fadump", early_fadump_param);

/*
 * Look for fadump_reserve_mem= cmdline option
 * TODO: Remove references to 'fadump_reserve_mem=' parameter,
 *       the sooner 'crashkernel=' parameter is accustomed to.
 */
static int __init early_fadump_reserve_mem(char *p)
{
	if (p)
		fw_dump.reserve_bootvar = memparse(p, &p);
	return 0;
}
early_param("fadump_reserve_mem", early_fadump_reserve_mem);

void crash_fadump(struct pt_regs *regs, const char *str)
{
	unsigned int msecs;
	struct fadump_crash_info_header *fdh = NULL;
	int old_cpu, this_cpu;
	/* Do not include first CPU */
	unsigned int ncpus = num_online_cpus() - 1;

	if (!should_fadump_crash())
		return;

	/*
	 * old_cpu == -1 means this is the first CPU which has come here,
	 * go ahead and trigger fadump.
	 *
	 * old_cpu != -1 means some other CPU has already on its way
	 * to trigger fadump, just keep looping here.
	 */
	this_cpu = smp_processor_id();
	old_cpu = cmpxchg(&crashing_cpu, -1, this_cpu);

	if (old_cpu != -1) {
		atomic_inc(&cpus_in_fadump);

		/*
		 * We can't loop here indefinitely. Wait as long as fadump
		 * is in force. If we race with fadump un-registration this
		 * loop will break and then we go down to normal panic path
		 * and reboot. If fadump is in force the first crashing
		 * cpu will definitely trigger fadump.
		 */
		while (fw_dump.dump_registered)
			cpu_relax();
		return;
	}

	fdh = __va(fw_dump.fadumphdr_addr);
	fdh->crashing_cpu = crashing_cpu;
	crash_save_vmcoreinfo();

	if (regs)
		fdh->regs = *regs;
	else
		ppc_save_regs(&fdh->regs);

	fdh->cpu_mask = *cpu_online_mask;

	/*
	 * If we came in via system reset, wait a while for the secondary
	 * CPUs to enter.
	 */
	if (TRAP(&(fdh->regs)) == INTERRUPT_SYSTEM_RESET) {
		msecs = CRASH_TIMEOUT;
		while ((atomic_read(&cpus_in_fadump) < ncpus) && (--msecs > 0))
			mdelay(1);
	}

	fw_dump.ops->fadump_trigger(fdh, str);
}

u32 *__init fadump_regs_to_elf_notes(u32 *buf, struct pt_regs *regs)
{
	struct elf_prstatus prstatus;

	memset(&prstatus, 0, sizeof(prstatus));
	/*
	 * FIXME: How do i get PID? Do I really need it?
	 * prstatus.pr_pid = ????
	 */
	elf_core_copy_regs(&prstatus.pr_reg, regs);
	buf = append_elf_note(buf, NN_PRSTATUS, NT_PRSTATUS,
			      &prstatus, sizeof(prstatus));
	return buf;
}

void __init fadump_update_elfcore_header(char *bufp)
{
	struct elf_phdr *phdr;

	bufp += sizeof(struct elfhdr);

	/* First note is a place holder for cpu notes info. */
	phdr = (struct elf_phdr *)bufp;

	if (phdr->p_type == PT_NOTE) {
		phdr->p_paddr	= __pa(fw_dump.cpu_notes_buf_vaddr);
		phdr->p_offset	= phdr->p_paddr;
		phdr->p_filesz	= fw_dump.cpu_notes_buf_size;
		phdr->p_memsz = fw_dump.cpu_notes_buf_size;
	}
	return;
}

static void *__init fadump_alloc_buffer(unsigned long size)
{
	unsigned long count, i;
	struct page *page;
	void *vaddr;

	vaddr = alloc_pages_exact(size, GFP_KERNEL | __GFP_ZERO);
	if (!vaddr)
		return NULL;

	count = PAGE_ALIGN(size) / PAGE_SIZE;
	page = virt_to_page(vaddr);
	for (i = 0; i < count; i++)
		mark_page_reserved(page + i);
	return vaddr;
}

static void fadump_free_buffer(unsigned long vaddr, unsigned long size)
{
	free_reserved_area((void *)vaddr, (void *)(vaddr + size), -1, NULL);
}

s32 __init fadump_setup_cpu_notes_buf(u32 num_cpus)
{
	/* Allocate buffer to hold cpu crash notes. */
	fw_dump.cpu_notes_buf_size = num_cpus * sizeof(note_buf_t);
	fw_dump.cpu_notes_buf_size = PAGE_ALIGN(fw_dump.cpu_notes_buf_size);
	fw_dump.cpu_notes_buf_vaddr =
		(unsigned long)fadump_alloc_buffer(fw_dump.cpu_notes_buf_size);
	if (!fw_dump.cpu_notes_buf_vaddr) {
		pr_err("Failed to allocate %ld bytes for CPU notes buffer\n",
		       fw_dump.cpu_notes_buf_size);
		return -ENOMEM;
	}

	pr_debug("Allocated buffer for cpu notes of size %ld at 0x%lx\n",
		 fw_dump.cpu_notes_buf_size,
		 fw_dump.cpu_notes_buf_vaddr);
	return 0;
}

void fadump_free_cpu_notes_buf(void)
{
	if (!fw_dump.cpu_notes_buf_vaddr)
		return;

	fadump_free_buffer(fw_dump.cpu_notes_buf_vaddr,
			   fw_dump.cpu_notes_buf_size);
	fw_dump.cpu_notes_buf_vaddr = 0;
	fw_dump.cpu_notes_buf_size = 0;
}

static void fadump_free_mem_ranges(struct fadump_mrange_info *mrange_info)
{
	if (mrange_info->is_static) {
		mrange_info->mem_range_cnt = 0;
		return;
	}

	kfree(mrange_info->mem_ranges);
	memset((void *)((u64)mrange_info + RNG_NAME_SZ), 0,
	       (sizeof(struct fadump_mrange_info) - RNG_NAME_SZ));
}

/*
 * Allocate or reallocate mem_ranges array in incremental units
 * of PAGE_SIZE.
 */
static int fadump_alloc_mem_ranges(struct fadump_mrange_info *mrange_info)
{
	struct fadump_memory_range *new_array;
	u64 new_size;

	new_size = mrange_info->mem_ranges_sz + PAGE_SIZE;
	pr_debug("Allocating %llu bytes of memory for %s memory ranges\n",
		 new_size, mrange_info->name);

	new_array = krealloc(mrange_info->mem_ranges, new_size, GFP_KERNEL);
	if (new_array == NULL) {
		pr_err("Insufficient memory for setting up %s memory ranges\n",
		       mrange_info->name);
		fadump_free_mem_ranges(mrange_info);
		return -ENOMEM;
	}

	mrange_info->mem_ranges = new_array;
	mrange_info->mem_ranges_sz = new_size;
	mrange_info->max_mem_ranges = (new_size /
				       sizeof(struct fadump_memory_range));
	return 0;
}
static inline int fadump_add_mem_range(struct fadump_mrange_info *mrange_info,
				       u64 base, u64 end)
{
	struct fadump_memory_range *mem_ranges = mrange_info->mem_ranges;
	bool is_adjacent = false;
	u64 start, size;

	if (base == end)
		return 0;

	/*
	 * Fold adjacent memory ranges to bring down the memory ranges/
	 * PT_LOAD segments count.
	 */
	if (mrange_info->mem_range_cnt) {
		start = mem_ranges[mrange_info->mem_range_cnt - 1].base;
		size  = mem_ranges[mrange_info->mem_range_cnt - 1].size;

		/*
		 * Boot memory area needs separate PT_LOAD segment(s) as it
		 * is moved to a different location at the time of crash.
		 * So, fold only if the region is not boot memory area.
		 */
		if ((start + size) == base && start >= fw_dump.boot_mem_top)
			is_adjacent = true;
	}
	if (!is_adjacent) {
		/* resize the array on reaching the limit */
		if (mrange_info->mem_range_cnt == mrange_info->max_mem_ranges) {
			int ret;

			if (mrange_info->is_static) {
				pr_err("Reached array size limit for %s memory ranges\n",
				       mrange_info->name);
				return -ENOSPC;
			}

			ret = fadump_alloc_mem_ranges(mrange_info);
			if (ret)
				return ret;

			/* Update to the new resized array */
			mem_ranges = mrange_info->mem_ranges;
		}

		start = base;
		mem_ranges[mrange_info->mem_range_cnt].base = start;
		mrange_info->mem_range_cnt++;
	}

	mem_ranges[mrange_info->mem_range_cnt - 1].size = (end - start);
	pr_debug("%s_memory_range[%d] [%#016llx-%#016llx], %#llx bytes\n",
		 mrange_info->name, (mrange_info->mem_range_cnt - 1),
		 start, end - 1, (end - start));
	return 0;
}

static int fadump_init_elfcore_header(char *bufp)
{
	struct elfhdr *elf;

	elf = (struct elfhdr *) bufp;
	bufp += sizeof(struct elfhdr);
	memcpy(elf->e_ident, ELFMAG, SELFMAG);
	elf->e_ident[EI_CLASS] = ELF_CLASS;
	elf->e_ident[EI_DATA] = ELF_DATA;
	elf->e_ident[EI_VERSION] = EV_CURRENT;
	elf->e_ident[EI_OSABI] = ELF_OSABI;
	memset(elf->e_ident+EI_PAD, 0, EI_NIDENT-EI_PAD);
	elf->e_type = ET_CORE;
	elf->e_machine = ELF_ARCH;
	elf->e_version = EV_CURRENT;
	elf->e_entry = 0;
	elf->e_phoff = sizeof(struct elfhdr);
	elf->e_shoff = 0;

	if (IS_ENABLED(CONFIG_PPC64_ELF_ABI_V2))
		elf->e_flags = 2;
	else if (IS_ENABLED(CONFIG_PPC64_ELF_ABI_V1))
		elf->e_flags = 1;
	else
		elf->e_flags = 0;

	elf->e_ehsize = sizeof(struct elfhdr);
	elf->e_phentsize = sizeof(struct elf_phdr);
	elf->e_phnum = 0;
	elf->e_shentsize = 0;
	elf->e_shnum = 0;
	elf->e_shstrndx = 0;

	return 0;
}

/*
 * If the given physical address falls within the boot memory region then
 * return the relocated address that points to the dump region reserved
 * for saving initial boot memory contents.
 */
static inline unsigned long fadump_relocate(unsigned long paddr)
{
	unsigned long raddr, rstart, rend, rlast, hole_size;
	int i;

	hole_size = 0;
	rlast = 0;
	raddr = paddr;
	for (i = 0; i < fw_dump.boot_mem_regs_cnt; i++) {
		rstart = fw_dump.boot_mem_addr[i];
		rend = rstart + fw_dump.boot_mem_sz[i];
		hole_size += (rstart - rlast);

		if (paddr >= rstart && paddr < rend) {
			raddr += fw_dump.boot_mem_dest_addr - hole_size;
			break;
		}

		rlast = rend;
	}

	pr_debug("vmcoreinfo: paddr = 0x%lx, raddr = 0x%lx\n", paddr, raddr);
	return raddr;
}

static void __init populate_elf_pt_load(struct elf_phdr *phdr, u64 start,
			     u64 size, unsigned long long offset)
{
	phdr->p_align	= 0;
	phdr->p_memsz	= size;
	phdr->p_filesz	= size;
	phdr->p_paddr	= start;
	phdr->p_offset	= offset;
	phdr->p_type	= PT_LOAD;
	phdr->p_flags	= PF_R|PF_W|PF_X;
	phdr->p_vaddr	= (unsigned long)__va(start);
}

static void __init fadump_populate_elfcorehdr(struct fadump_crash_info_header *fdh)
{
	char *bufp;
	struct elfhdr *elf;
	struct elf_phdr *phdr;
	u64 boot_mem_dest_offset;
	unsigned long long i, ra_start, ra_end, ra_size, mstart, mend;

	bufp = (char *) fw_dump.elfcorehdr_addr;
	fadump_init_elfcore_header(bufp);
	elf = (struct elfhdr *)bufp;
	bufp += sizeof(struct elfhdr);

	/*
	 * Set up ELF PT_NOTE, a placeholder for CPU notes information.
	 * The notes info will be populated later by platform-specific code.
	 * Hence, this PT_NOTE will always be the first ELF note.
	 *
	 * NOTE: Any new ELF note addition should be placed after this note.
	 */
	phdr = (struct elf_phdr *)bufp;
	bufp += sizeof(struct elf_phdr);
	phdr->p_type = PT_NOTE;
	phdr->p_flags	= 0;
	phdr->p_vaddr	= 0;
	phdr->p_align	= 0;
	phdr->p_offset	= 0;
	phdr->p_paddr	= 0;
	phdr->p_filesz	= 0;
	phdr->p_memsz	= 0;
	/* Increment number of program headers. */
	(elf->e_phnum)++;

	/* setup ELF PT_NOTE for vmcoreinfo */
	phdr = (struct elf_phdr *)bufp;
	bufp += sizeof(struct elf_phdr);
	phdr->p_type	= PT_NOTE;
	phdr->p_flags	= 0;
	phdr->p_vaddr	= 0;
	phdr->p_align	= 0;
	phdr->p_paddr	= phdr->p_offset = fdh->vmcoreinfo_raddr;
	phdr->p_memsz	= phdr->p_filesz = fdh->vmcoreinfo_size;
	/* Increment number of program headers. */
	(elf->e_phnum)++;

	/*
	 * Setup PT_LOAD sections. first include boot memory regions
	 * and then add rest of the memory regions.
	 */
	boot_mem_dest_offset = fw_dump.boot_mem_dest_addr;
	for (i = 0; i < fw_dump.boot_mem_regs_cnt; i++) {
		phdr = (struct elf_phdr *)bufp;
		bufp += sizeof(struct elf_phdr);
		populate_elf_pt_load(phdr, fw_dump.boot_mem_addr[i],
				     fw_dump.boot_mem_sz[i],
				     boot_mem_dest_offset);
		/* Increment number of program headers. */
		(elf->e_phnum)++;
		boot_mem_dest_offset += fw_dump.boot_mem_sz[i];
	}

	/* Memory reserved for fadump in first kernel */
	ra_start = fw_dump.reserve_dump_area_start;
	ra_size = get_fadump_area_size();
	ra_end = ra_start + ra_size;

	phdr = (struct elf_phdr *)bufp;
	for_each_mem_range(i, &mstart, &mend) {
		/* Boot memory regions already added, skip them now */
		if (mstart < fw_dump.boot_mem_top) {
			if (mend > fw_dump.boot_mem_top)
				mstart = fw_dump.boot_mem_top;
			else
				continue;
		}

		/* Handle memblock regions overlaps with fadump reserved area */
		if ((ra_start < mend) && (ra_end > mstart)) {
			if ((mstart < ra_start) && (mend > ra_end)) {
				populate_elf_pt_load(phdr, mstart, ra_start - mstart, mstart);
				/* Increment number of program headers. */
				(elf->e_phnum)++;
				bufp += sizeof(struct elf_phdr);
				phdr = (struct elf_phdr *)bufp;
				populate_elf_pt_load(phdr, ra_end, mend - ra_end, ra_end);
			} else if (mstart < ra_start) {
				populate_elf_pt_load(phdr, mstart, ra_start - mstart, mstart);
			} else if (ra_end < mend) {
				populate_elf_pt_load(phdr, ra_end, mend - ra_end, ra_end);
			}
		} else {
		/* No overlap with fadump reserved memory region */
			populate_elf_pt_load(phdr, mstart, mend - mstart, mstart);
		}

		/* Increment number of program headers. */
		(elf->e_phnum)++;
		bufp += sizeof(struct elf_phdr);
		phdr = (struct elf_phdr *) bufp;
	}
}

static unsigned long init_fadump_header(unsigned long addr)
{
	struct fadump_crash_info_header *fdh;

	if (!addr)
		return 0;

	fdh = __va(addr);
	addr += sizeof(struct fadump_crash_info_header);

	memset(fdh, 0, sizeof(struct fadump_crash_info_header));
	fdh->magic_number = FADUMP_CRASH_INFO_MAGIC;
	fdh->version = FADUMP_HEADER_VERSION;
	/* We will set the crashing cpu id in crash_fadump() during crash. */
	fdh->crashing_cpu = FADUMP_CPU_UNKNOWN;

	/*
	 * The physical address and size of vmcoreinfo are required in the
	 * second kernel to prepare elfcorehdr.
	 */
	fdh->vmcoreinfo_raddr = fadump_relocate(paddr_vmcoreinfo_note());
	fdh->vmcoreinfo_size = VMCOREINFO_NOTE_SIZE;


	fdh->pt_regs_sz = sizeof(struct pt_regs);
	/*
	 * When LPAR is terminated by PYHP, ensure all possible CPUs'
	 * register data is processed while exporting the vmcore.
	 */
	fdh->cpu_mask = *cpu_possible_mask;
	fdh->cpu_mask_sz = sizeof(struct cpumask);

	return addr;
}

static int register_fadump(void)
{
	unsigned long addr;

	/*
	 * If no memory is reserved then we can not register for firmware-
	 * assisted dump.
	 */
	if (!fw_dump.reserve_dump_area_size)
		return -ENODEV;

	addr = fw_dump.fadumphdr_addr;

	/* Initialize fadump crash info header. */
	addr = init_fadump_header(addr);

	/* register the future kernel dump with firmware. */
	pr_debug("Registering for firmware-assisted kernel dump...\n");
	return fw_dump.ops->fadump_register(&fw_dump);
}

void fadump_cleanup(void)
{
	if (!fw_dump.fadump_supported)
		return;

	/* Invalidate the registration only if dump is active. */
	if (fw_dump.dump_active) {
		pr_debug("Invalidating firmware-assisted dump registration\n");
		fw_dump.ops->fadump_invalidate(&fw_dump);
	} else if (fw_dump.dump_registered) {
		/* Un-register Firmware-assisted dump if it was registered. */
		fw_dump.ops->fadump_unregister(&fw_dump);
	}

	if (fw_dump.ops->fadump_cleanup)
		fw_dump.ops->fadump_cleanup(&fw_dump);
}

static void fadump_free_reserved_memory(unsigned long start_pfn,
					unsigned long end_pfn)
{
	unsigned long pfn;
	unsigned long time_limit = jiffies + HZ;

	pr_info("freeing reserved memory (0x%llx - 0x%llx)\n",
		PFN_PHYS(start_pfn), PFN_PHYS(end_pfn));

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		free_reserved_page(pfn_to_page(pfn));

		if (time_after(jiffies, time_limit)) {
			cond_resched();
			time_limit = jiffies + HZ;
		}
	}
}

/*
 * Skip memory holes and free memory that was actually reserved.
 */
static void fadump_release_reserved_area(u64 start, u64 end)
{
	unsigned long reg_spfn, reg_epfn;
	u64 tstart, tend, spfn, epfn;
	int i;

	spfn = PHYS_PFN(start);
	epfn = PHYS_PFN(end);

	for_each_mem_pfn_range(i, MAX_NUMNODES, &reg_spfn, &reg_epfn, NULL) {
		tstart = max_t(u64, spfn, reg_spfn);
		tend   = min_t(u64, epfn, reg_epfn);

		if (tstart < tend) {
			fadump_free_reserved_memory(tstart, tend);

			if (tend == epfn)
				break;

			spfn = tend;
		}
	}
}

/*
 * Sort the mem ranges in-place and merge adjacent ranges
 * to minimize the memory ranges count.
 */
static void sort_and_merge_mem_ranges(struct fadump_mrange_info *mrange_info)
{
	struct fadump_memory_range *mem_ranges;
	u64 base, size;
	int i, j, idx;

	if (!reserved_mrange_info.mem_range_cnt)
		return;

	/* Sort the memory ranges */
	mem_ranges = mrange_info->mem_ranges;
	for (i = 0; i < mrange_info->mem_range_cnt; i++) {
		idx = i;
		for (j = (i + 1); j < mrange_info->mem_range_cnt; j++) {
			if (mem_ranges[idx].base > mem_ranges[j].base)
				idx = j;
		}
		if (idx != i)
			swap(mem_ranges[idx], mem_ranges[i]);
	}

	/* Merge adjacent reserved ranges */
	idx = 0;
	for (i = 1; i < mrange_info->mem_range_cnt; i++) {
		base = mem_ranges[i-1].base;
		size = mem_ranges[i-1].size;
		if (mem_ranges[i].base == (base + size))
			mem_ranges[idx].size += mem_ranges[i].size;
		else {
			idx++;
			if (i == idx)
				continue;

			mem_ranges[idx] = mem_ranges[i];
		}
	}
	mrange_info->mem_range_cnt = idx + 1;
}

/*
 * Scan reserved-ranges to consider them while reserving/releasing
 * memory for FADump.
 */
static void __init early_init_dt_scan_reserved_ranges(unsigned long node)
{
	const __be32 *prop;
	int len, ret = -1;
	unsigned long i;

	/* reserved-ranges already scanned */
	if (reserved_mrange_info.mem_range_cnt != 0)
		return;

	prop = of_get_flat_dt_prop(node, "reserved-ranges", &len);
	if (!prop)
		return;

	/*
	 * Each reserved range is an (address,size) pair, 2 cells each,
	 * totalling 4 cells per range.
	 */
	for (i = 0; i < len / (sizeof(*prop) * 4); i++) {
		u64 base, size;

		base = of_read_number(prop + (i * 4) + 0, 2);
		size = of_read_number(prop + (i * 4) + 2, 2);

		if (size) {
			ret = fadump_add_mem_range(&reserved_mrange_info,
						   base, base + size);
			if (ret < 0) {
				pr_warn("some reserved ranges are ignored!\n");
				break;
			}
		}
	}

	/* Compact reserved ranges */
	sort_and_merge_mem_ranges(&reserved_mrange_info);
}

/*
 * Release the memory that was reserved during early boot to preserve the
 * crash'ed kernel's memory contents except reserved dump area (permanent
 * reservation) and reserved ranges used by F/W. The released memory will
 * be available for general use.
 */
static void fadump_release_memory(u64 begin, u64 end)
{
	u64 ra_start, ra_end, tstart;
	int i, ret;

	ra_start = fw_dump.reserve_dump_area_start;
	ra_end = ra_start + fw_dump.reserve_dump_area_size;

	/*
	 * If reserved ranges array limit is hit, overwrite the last reserved
	 * memory range with reserved dump area to ensure it is excluded from
	 * the memory being released (reused for next FADump registration).
	 */
	if (reserved_mrange_info.mem_range_cnt ==
	    reserved_mrange_info.max_mem_ranges)
		reserved_mrange_info.mem_range_cnt--;

	ret = fadump_add_mem_range(&reserved_mrange_info, ra_start, ra_end);
	if (ret != 0)
		return;

	/* Get the reserved ranges list in order first. */
	sort_and_merge_mem_ranges(&reserved_mrange_info);

	/* Exclude reserved ranges and release remaining memory */
	tstart = begin;
	for (i = 0; i < reserved_mrange_info.mem_range_cnt; i++) {
		ra_start = reserved_mrange_info.mem_ranges[i].base;
		ra_end = ra_start + reserved_mrange_info.mem_ranges[i].size;

		if (tstart >= ra_end)
			continue;

		if (tstart < ra_start)
			fadump_release_reserved_area(tstart, ra_start);
		tstart = ra_end;
	}

	if (tstart < end)
		fadump_release_reserved_area(tstart, end);
}

static void fadump_free_elfcorehdr_buf(void)
{
	if (fw_dump.elfcorehdr_addr == 0 || fw_dump.elfcorehdr_size == 0)
		return;

	/*
	 * Before freeing the memory of `elfcorehdr`, reset the global
	 * `elfcorehdr_addr` to prevent modules like `vmcore` from accessing
	 * invalid memory.
	 */
	elfcorehdr_addr = ELFCORE_ADDR_ERR;
	fadump_free_buffer(fw_dump.elfcorehdr_addr, fw_dump.elfcorehdr_size);
	fw_dump.elfcorehdr_addr = 0;
	fw_dump.elfcorehdr_size = 0;
}

static void fadump_invalidate_release_mem(void)
{
	scoped_guard(mutex, &fadump_mutex) {
		if (!fw_dump.dump_active)
			return;
		fadump_cleanup();
	}

	fadump_free_elfcorehdr_buf();
	fadump_release_memory(fw_dump.boot_mem_top, memblock_end_of_DRAM());
	fadump_free_cpu_notes_buf();

	/*
	 * Setup kernel metadata and initialize the kernel dump
	 * memory structure for FADump re-registration.
	 */
	if (fw_dump.ops->fadump_setup_metadata &&
	    (fw_dump.ops->fadump_setup_metadata(&fw_dump) < 0))
		pr_warn("Failed to setup kernel metadata!\n");
	fw_dump.ops->fadump_init_mem_struct(&fw_dump);
}

static ssize_t release_mem_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	int input = -1;

	if (!fw_dump.dump_active)
		return -EPERM;

	if (kstrtoint(buf, 0, &input))
		return -EINVAL;

	if (input == 1) {
		/*
		 * Take away the '/proc/vmcore'. We are releasing the dump
		 * memory, hence it will not be valid anymore.
		 */
#ifdef CONFIG_PROC_VMCORE
		vmcore_cleanup();
#endif
		fadump_invalidate_release_mem();

	} else
		return -EINVAL;
	return count;
}

/* Release the reserved memory and disable the FADump */
static void __init unregister_fadump(void)
{
	fadump_cleanup();
	fadump_release_memory(fw_dump.reserve_dump_area_start,
			      fw_dump.reserve_dump_area_size);
	fw_dump.fadump_enabled = 0;
	kobject_put(fadump_kobj);
}

static ssize_t enabled_show(struct kobject *kobj,
			    struct kobj_attribute *attr,
			    char *buf)
{
	return sprintf(buf, "%d\n", fw_dump.fadump_enabled);
}

/*
 * /sys/kernel/fadump/hotplug_ready sysfs node returns 1, which inidcates
 * to usersapce that fadump re-registration is not required on memory
 * hotplug events.
 */
static ssize_t hotplug_ready_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      char *buf)
{
	return sprintf(buf, "%d\n", 1);
}

static ssize_t mem_reserved_show(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	return sprintf(buf, "%ld\n", fw_dump.reserve_dump_area_size);
}

static ssize_t registered_show(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       char *buf)
{
	return sprintf(buf, "%d\n", fw_dump.dump_registered);
}

static ssize_t bootargs_append_show(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   char *buf)
{
	return sprintf(buf, "%s\n", (char *)__va(fw_dump.param_area));
}

static ssize_t bootargs_append_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	char *params;

	if (!fw_dump.fadump_enabled || fw_dump.dump_active)
		return -EPERM;

	if (count >= COMMAND_LINE_SIZE)
		return -EINVAL;

	/*
	 * Fail here instead of handling this scenario with
	 * some silly workaround in capture kernel.
	 */
	if (saved_command_line_len + count >= COMMAND_LINE_SIZE) {
		pr_err("Appending parameters exceeds cmdline size!\n");
		return -ENOSPC;
	}

	params = __va(fw_dump.param_area);
	strscpy_pad(params, buf, COMMAND_LINE_SIZE);
	/* Remove newline character at the end. */
	if (params[count-1] == '\n')
		params[count-1] = '\0';

	return count;
}

static ssize_t registered_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	int ret = 0;
	int input = -1;

	if (!fw_dump.fadump_enabled || fw_dump.dump_active)
		return -EPERM;

	if (kstrtoint(buf, 0, &input))
		return -EINVAL;

	mutex_lock(&fadump_mutex);

	switch (input) {
	case 0:
		if (fw_dump.dump_registered == 0) {
			goto unlock_out;
		}

		/* Un-register Firmware-assisted dump */
		pr_debug("Un-register firmware-assisted dump\n");
		fw_dump.ops->fadump_unregister(&fw_dump);
		break;
	case 1:
		if (fw_dump.dump_registered == 1) {
			/* Un-register Firmware-assisted dump */
			fw_dump.ops->fadump_unregister(&fw_dump);
		}
		/* Register Firmware-assisted dump */
		ret = register_fadump();
		break;
	default:
		ret = -EINVAL;
		break;
	}

unlock_out:
	mutex_unlock(&fadump_mutex);
	return ret < 0 ? ret : count;
}

static int fadump_region_show(struct seq_file *m, void *private)
{
	if (!fw_dump.fadump_enabled)
		return 0;

	mutex_lock(&fadump_mutex);
	fw_dump.ops->fadump_region_show(&fw_dump, m);
	mutex_unlock(&fadump_mutex);
	return 0;
}

static struct kobj_attribute release_attr = __ATTR_WO(release_mem);
static struct kobj_attribute enable_attr = __ATTR_RO(enabled);
static struct kobj_attribute register_attr = __ATTR_RW(registered);
static struct kobj_attribute mem_reserved_attr = __ATTR_RO(mem_reserved);
static struct kobj_attribute hotplug_ready_attr = __ATTR_RO(hotplug_ready);
static struct kobj_attribute bootargs_append_attr = __ATTR_RW(bootargs_append);

static struct attribute *fadump_attrs[] = {
	&enable_attr.attr,
	&register_attr.attr,
	&mem_reserved_attr.attr,
	&hotplug_ready_attr.attr,
	NULL,
};

ATTRIBUTE_GROUPS(fadump);

DEFINE_SHOW_ATTRIBUTE(fadump_region);

static void __init fadump_init_files(void)
{
	int rc = 0;

	fadump_kobj = kobject_create_and_add("fadump", kernel_kobj);
	if (!fadump_kobj) {
		pr_err("failed to create fadump kobject\n");
		return;
	}

	if (fw_dump.param_area) {
		rc = sysfs_create_file(fadump_kobj, &bootargs_append_attr.attr);
		if (rc)
			pr_err("unable to create bootargs_append sysfs file (%d)\n", rc);
	}

	debugfs_create_file("fadump_region", 0444, arch_debugfs_dir, NULL,
			    &fadump_region_fops);

	if (fw_dump.dump_active) {
		rc = sysfs_create_file(fadump_kobj, &release_attr.attr);
		if (rc)
			pr_err("unable to create release_mem sysfs file (%d)\n",
			       rc);
	}

	rc = sysfs_create_groups(fadump_kobj, fadump_groups);
	if (rc) {
		pr_err("sysfs group creation failed (%d), unregistering FADump",
		       rc);
		unregister_fadump();
		return;
	}

	/*
	 * The FADump sysfs are moved from kernel_kobj to fadump_kobj need to
	 * create symlink at old location to maintain backward compatibility.
	 *
	 *      - fadump_enabled -> fadump/enabled
	 *      - fadump_registered -> fadump/registered
	 *      - fadump_release_mem -> fadump/release_mem
	 */
	rc = compat_only_sysfs_link_entry_to_kobj(kernel_kobj, fadump_kobj,
						  "enabled", "fadump_enabled");
	if (rc) {
		pr_err("unable to create fadump_enabled symlink (%d)", rc);
		return;
	}

	rc = compat_only_sysfs_link_entry_to_kobj(kernel_kobj, fadump_kobj,
						  "registered",
						  "fadump_registered");
	if (rc) {
		pr_err("unable to create fadump_registered symlink (%d)", rc);
		sysfs_remove_link(kernel_kobj, "fadump_enabled");
		return;
	}

	if (fw_dump.dump_active) {
		rc = compat_only_sysfs_link_entry_to_kobj(kernel_kobj,
							  fadump_kobj,
							  "release_mem",
							  "fadump_release_mem");
		if (rc)
			pr_err("unable to create fadump_release_mem symlink (%d)",
			       rc);
	}
	return;
}

static int __init fadump_setup_elfcorehdr_buf(void)
{
	int elf_phdr_cnt;
	unsigned long elfcorehdr_size;

	/*
	 * Program header for CPU notes comes first, followed by one for
	 * vmcoreinfo, and the remaining program headers correspond to
	 * memory regions.
	 */
	elf_phdr_cnt = 2 + fw_dump.boot_mem_regs_cnt + memblock_num_regions(memory);
	elfcorehdr_size = sizeof(struct elfhdr) + (elf_phdr_cnt * sizeof(struct elf_phdr));
	elfcorehdr_size = PAGE_ALIGN(elfcorehdr_size);

	fw_dump.elfcorehdr_addr = (u64)fadump_alloc_buffer(elfcorehdr_size);
	if (!fw_dump.elfcorehdr_addr) {
		pr_err("Failed to allocate %lu bytes for elfcorehdr\n",
		       elfcorehdr_size);
		return -ENOMEM;
	}
	fw_dump.elfcorehdr_size = elfcorehdr_size;
	return 0;
}

/*
 * Check if the fadump header of crashed kernel is compatible with fadump kernel.
 *
 * It checks the magic number, endianness, and size of non-primitive type
 * members of fadump header to ensure safe dump collection.
 */
static bool __init is_fadump_header_compatible(struct fadump_crash_info_header *fdh)
{
	if (fdh->magic_number == FADUMP_CRASH_INFO_MAGIC_OLD) {
		pr_err("Old magic number, can't process the dump.\n");
		return false;
	}

	if (fdh->magic_number != FADUMP_CRASH_INFO_MAGIC) {
		if (fdh->magic_number == swab64(FADUMP_CRASH_INFO_MAGIC))
			pr_err("Endianness mismatch between the crashed and fadump kernels.\n");
		else
			pr_err("Fadump header is corrupted.\n");

		return false;
	}

	/*
	 * Dump collection is not safe if the size of non-primitive type members
	 * of the fadump header do not match between crashed and fadump kernel.
	 */
	if (fdh->pt_regs_sz != sizeof(struct pt_regs) ||
	    fdh->cpu_mask_sz != sizeof(struct cpumask)) {
		pr_err("Fadump header size mismatch.\n");
		return false;
	}

	return true;
}

static void __init fadump_process(void)
{
	struct fadump_crash_info_header *fdh;

	fdh = (struct fadump_crash_info_header *) __va(fw_dump.fadumphdr_addr);
	if (!fdh) {
		pr_err("Crash info header is empty.\n");
		goto err_out;
	}

	/* Avoid processing the dump if fadump header isn't compatible */
	if (!is_fadump_header_compatible(fdh))
		goto err_out;

	/* Allocate buffer for elfcorehdr */
	if (fadump_setup_elfcorehdr_buf())
		goto err_out;

	fadump_populate_elfcorehdr(fdh);

	/* Let platform update the CPU notes in elfcorehdr */
	if (fw_dump.ops->fadump_process(&fw_dump) < 0)
		goto err_out;

	/*
	 * elfcorehdr is now ready to be exported.
	 *
	 * set elfcorehdr_addr so that vmcore module will export the
	 * elfcorehdr through '/proc/vmcore'.
	 */
	elfcorehdr_addr = virt_to_phys((void *)fw_dump.elfcorehdr_addr);
	return;

err_out:
	fadump_invalidate_release_mem();
}

/*
 * Reserve memory to store additional parameters to be passed
 * for fadump/capture kernel.
 */
void __init fadump_setup_param_area(void)
{
	phys_addr_t range_start, range_end;

	if (!fw_dump.param_area_supported || fw_dump.dump_active)
		return;

	/* This memory can't be used by PFW or bootloader as it is shared across kernels */
	if (early_radix_enabled()) {
		/*
		 * Anywhere in the upper half should be good enough as all memory
		 * is accessible in real mode.
		 */
		range_start = memblock_end_of_DRAM() / 2;
		range_end = memblock_end_of_DRAM();
	} else {
		/*
		 * Memory range for passing additional parameters for HASH MMU
		 * must meet the following conditions:
		 * 1. The first memory block size must be higher than the
		 *    minimum RMA (MIN_RMA) size. Bootloader can use memory
		 *    upto RMA size. So it should be avoided.
		 * 2. The range should be between MIN_RMA and RMA size (ppc64_rma_size)
		 * 3. It must not overlap with the fadump reserved area.
		 */
		if (ppc64_rma_size < MIN_RMA*1024*1024)
			return;

		range_start = MIN_RMA * 1024 * 1024;
		range_end = min(ppc64_rma_size, fw_dump.boot_mem_top);
	}

	fw_dump.param_area = memblock_phys_alloc_range(COMMAND_LINE_SIZE,
						       COMMAND_LINE_SIZE,
						       range_start,
						       range_end);
	if (!fw_dump.param_area) {
		pr_warn("WARNING: Could not setup area to pass additional parameters!\n");
		return;
	}

	memset((void *)fw_dump.param_area, 0, COMMAND_LINE_SIZE);
}

/*
 * Prepare for firmware-assisted dump.
 */
int __init setup_fadump(void)
{
	if (!fw_dump.fadump_supported)
		return 0;

	fadump_init_files();
	fadump_show_config();

	if (!fw_dump.fadump_enabled)
		return 1;

	/*
	 * If dump data is available then see if it is valid and prepare for
	 * saving it to the disk.
	 */
	if (fw_dump.dump_active) {
		fadump_process();
	}
	/* Initialize the kernel dump memory structure and register with f/w */
	else if (fw_dump.reserve_dump_area_size) {
		fw_dump.ops->fadump_init_mem_struct(&fw_dump);
		register_fadump();
	}

	/*
	 * In case of panic, fadump is triggered via ppc_panic_event()
	 * panic notifier. Setting crash_kexec_post_notifiers to 'true'
	 * lets panic() function take crash friendly path before panic
	 * notifiers are invoked.
	 */
	crash_kexec_post_notifiers = true;

	return 1;
}
/*
 * Use subsys_initcall_sync() here because there is dependency with
 * crash_save_vmcoreinfo_init(), which must run first to ensure vmcoreinfo initialization
 * is done before registering with f/w.
 */
subsys_initcall_sync(setup_fadump);
#else /* !CONFIG_PRESERVE_FA_DUMP */

/* Scan the Firmware Assisted dump configuration details. */
int __init early_init_dt_scan_fw_dump(unsigned long node, const char *uname,
				      int depth, void *data)
{
	if ((depth != 1) || (strcmp(uname, "ibm,opal") != 0))
		return 0;

	opal_fadump_dt_scan(&fw_dump, node);
	return 1;
}

/*
 * When dump is active but PRESERVE_FA_DUMP is enabled on the kernel,
 * preserve crash data. The subsequent memory preserving kernel boot
 * is likely to process this crash data.
 */
int __init fadump_reserve_mem(void)
{
	if (fw_dump.dump_active) {
		/*
		 * If last boot has crashed then reserve all the memory
		 * above boot memory to preserve crash data.
		 */
		pr_info("Preserving crash data for processing in next boot.\n");
		fadump_reserve_crash_area(fw_dump.boot_mem_top);
	} else
		pr_debug("FADump-aware kernel..\n");

	return 1;
}
#endif /* CONFIG_PRESERVE_FA_DUMP */

/* Preserve everything above the base address */
static void __init fadump_reserve_crash_area(u64 base)
{
	u64 i, mstart, mend, msize;

	for_each_mem_range(i, &mstart, &mend) {
		msize  = mend - mstart;

		if ((mstart + msize) < base)
			continue;

		if (mstart < base) {
			msize -= (base - mstart);
			mstart = base;
		}

		pr_info("Reserving %lluMB of memory at %#016llx for preserving crash data",
			(msize >> 20), mstart);
		memblock_reserve(mstart, msize);
	}
}
