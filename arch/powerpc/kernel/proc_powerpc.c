// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2001 Mike Corrigan & Dave Engebretsen IBM Corporation
 */

#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/string.h>

#include <asm/machdep.h>
#include <asm/vdso_datapage.h>
#include <asm/rtas.h>
#include <asm/systemcfg.h>
#include <linux/uaccess.h>

#ifdef CONFIG_PPC64_PROC_SYSTEMCFG

static loff_t page_map_seek(struct file *file, loff_t off, int whence)
{
	return fixed_size_llseek(file, off, whence, PAGE_SIZE);
}

static ssize_t page_map_read( struct file *file, char __user *buf, size_t nbytes,
			      loff_t *ppos)
{
	return simple_read_from_buffer(buf, nbytes, ppos,
			pde_data(file_inode(file)), PAGE_SIZE);
}

static int page_map_mmap( struct file *file, struct vm_area_struct *vma )
{
	if ((vma->vm_end - vma->vm_start) > PAGE_SIZE)
		return -EINVAL;

	return remap_pfn_range(vma, vma->vm_start,
			       __pa(pde_data(file_inode(file))) >> PAGE_SHIFT,
			       PAGE_SIZE, vma->vm_page_prot);
}

static const struct proc_ops page_map_proc_ops = {
	.proc_lseek	= page_map_seek,
	.proc_read	= page_map_read,
	.proc_mmap	= page_map_mmap,
};

static union {
	struct systemcfg	data;
	u8			page[PAGE_SIZE];
} systemcfg_data_store __page_aligned_data;
struct systemcfg *systemcfg = &systemcfg_data_store.data;

static int __init proc_ppc64_init(void)
{
	struct proc_dir_entry *pde;

	strscpy(systemcfg->eye_catcher, "SYSTEMCFG:PPC64");
	systemcfg->version.major = SYSTEMCFG_MAJOR;
	systemcfg->version.minor = SYSTEMCFG_MINOR;
	systemcfg->processor = mfspr(SPRN_PVR);
	/*
	 * Fake the old platform number for pSeries and add
	 * in LPAR bit if necessary
	 */
	systemcfg->platform = 0x100;
	if (firmware_has_feature(FW_FEATURE_LPAR))
		systemcfg->platform |= 1;
	systemcfg->physicalMemorySize = memblock_phys_mem_size();
	systemcfg->dcache_size = ppc64_caches.l1d.size;
	systemcfg->dcache_line_size = ppc64_caches.l1d.line_size;
	systemcfg->icache_size = ppc64_caches.l1i.size;
	systemcfg->icache_line_size = ppc64_caches.l1i.line_size;

	pde = proc_create_data("powerpc/systemcfg", S_IFREG | 0444, NULL,
			       &page_map_proc_ops, systemcfg);
	if (!pde)
		return 1;
	proc_set_size(pde, PAGE_SIZE);

	return 0;
}
__initcall(proc_ppc64_init);

#endif /* CONFIG_PPC64_PROC_SYSTEMCFG */

/*
 * Create the ppc64 and ppc64/rtas directories early. This allows us to
 * assume that they have been previously created in drivers.
 */
static int __init proc_ppc64_create(void)
{
	struct proc_dir_entry *root;

	root = proc_mkdir("powerpc", NULL);
	if (!root)
		return 1;

#ifdef CONFIG_PPC64
	if (!proc_symlink("ppc64", NULL, "powerpc"))
		pr_err("Failed to create link /proc/ppc64 -> /proc/powerpc\n");
#endif

	if (!of_find_node_by_path("/rtas"))
		return 0;

	if (!proc_mkdir("rtas", root))
		return 1;

	if (!proc_symlink("rtas", NULL, "powerpc/rtas"))
		return 1;

	return 0;
}
core_initcall(proc_ppc64_create);
