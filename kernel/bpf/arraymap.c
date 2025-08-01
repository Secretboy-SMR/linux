// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2011-2014 PLUMgrid, http://plumgrid.com
 * Copyright (c) 2016,2017 Facebook
 */
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/filter.h>
#include <linux/perf_event.h>
#include <uapi/linux/btf.h>
#include <linux/rcupdate_trace.h>
#include <linux/btf_ids.h>

#include "map_in_map.h"

#define ARRAY_CREATE_FLAG_MASK \
	(BPF_F_NUMA_NODE | BPF_F_MMAPABLE | BPF_F_ACCESS_MASK | \
	 BPF_F_PRESERVE_ELEMS | BPF_F_INNER_MAP)

static void bpf_array_free_percpu(struct bpf_array *array)
{
	int i;

	for (i = 0; i < array->map.max_entries; i++) {
		free_percpu(array->pptrs[i]);
		cond_resched();
	}
}

static int bpf_array_alloc_percpu(struct bpf_array *array)
{
	void __percpu *ptr;
	int i;

	for (i = 0; i < array->map.max_entries; i++) {
		ptr = bpf_map_alloc_percpu(&array->map, array->elem_size, 8,
					   GFP_USER | __GFP_NOWARN);
		if (!ptr) {
			bpf_array_free_percpu(array);
			return -ENOMEM;
		}
		array->pptrs[i] = ptr;
		cond_resched();
	}

	return 0;
}

/* Called from syscall */
int array_map_alloc_check(union bpf_attr *attr)
{
	bool percpu = attr->map_type == BPF_MAP_TYPE_PERCPU_ARRAY;
	int numa_node = bpf_map_attr_numa_node(attr);

	/* check sanity of attributes */
	if (attr->max_entries == 0 || attr->key_size != 4 ||
	    attr->value_size == 0 ||
	    attr->map_flags & ~ARRAY_CREATE_FLAG_MASK ||
	    !bpf_map_flags_access_ok(attr->map_flags) ||
	    (percpu && numa_node != NUMA_NO_NODE))
		return -EINVAL;

	if (attr->map_type != BPF_MAP_TYPE_ARRAY &&
	    attr->map_flags & (BPF_F_MMAPABLE | BPF_F_INNER_MAP))
		return -EINVAL;

	if (attr->map_type != BPF_MAP_TYPE_PERF_EVENT_ARRAY &&
	    attr->map_flags & BPF_F_PRESERVE_ELEMS)
		return -EINVAL;

	/* avoid overflow on round_up(map->value_size) */
	if (attr->value_size > INT_MAX)
		return -E2BIG;
	/* percpu map value size is bound by PCPU_MIN_UNIT_SIZE */
	if (percpu && round_up(attr->value_size, 8) > PCPU_MIN_UNIT_SIZE)
		return -E2BIG;

	return 0;
}

static struct bpf_map *array_map_alloc(union bpf_attr *attr)
{
	bool percpu = attr->map_type == BPF_MAP_TYPE_PERCPU_ARRAY;
	int numa_node = bpf_map_attr_numa_node(attr);
	u32 elem_size, index_mask, max_entries;
	bool bypass_spec_v1 = bpf_bypass_spec_v1(NULL);
	u64 array_size, mask64;
	struct bpf_array *array;

	elem_size = round_up(attr->value_size, 8);

	max_entries = attr->max_entries;

	/* On 32 bit archs roundup_pow_of_two() with max_entries that has
	 * upper most bit set in u32 space is undefined behavior due to
	 * resulting 1U << 32, so do it manually here in u64 space.
	 */
	mask64 = fls_long(max_entries - 1);
	mask64 = 1ULL << mask64;
	mask64 -= 1;

	index_mask = mask64;
	if (!bypass_spec_v1) {
		/* round up array size to nearest power of 2,
		 * since cpu will speculate within index_mask limits
		 */
		max_entries = index_mask + 1;
		/* Check for overflows. */
		if (max_entries < attr->max_entries)
			return ERR_PTR(-E2BIG);
	}

	array_size = sizeof(*array);
	if (percpu) {
		array_size += (u64) max_entries * sizeof(void *);
	} else {
		/* rely on vmalloc() to return page-aligned memory and
		 * ensure array->value is exactly page-aligned
		 */
		if (attr->map_flags & BPF_F_MMAPABLE) {
			array_size = PAGE_ALIGN(array_size);
			array_size += PAGE_ALIGN((u64) max_entries * elem_size);
		} else {
			array_size += (u64) max_entries * elem_size;
		}
	}

	/* allocate all map elements and zero-initialize them */
	if (attr->map_flags & BPF_F_MMAPABLE) {
		void *data;

		/* kmalloc'ed memory can't be mmap'ed, use explicit vmalloc */
		data = bpf_map_area_mmapable_alloc(array_size, numa_node);
		if (!data)
			return ERR_PTR(-ENOMEM);
		array = data + PAGE_ALIGN(sizeof(struct bpf_array))
			- offsetof(struct bpf_array, value);
	} else {
		array = bpf_map_area_alloc(array_size, numa_node);
	}
	if (!array)
		return ERR_PTR(-ENOMEM);
	array->index_mask = index_mask;
	array->map.bypass_spec_v1 = bypass_spec_v1;

	/* copy mandatory map attributes */
	bpf_map_init_from_attr(&array->map, attr);
	array->elem_size = elem_size;

	if (percpu && bpf_array_alloc_percpu(array)) {
		bpf_map_area_free(array);
		return ERR_PTR(-ENOMEM);
	}

	return &array->map;
}

static void *array_map_elem_ptr(struct bpf_array* array, u32 index)
{
	return array->value + (u64)array->elem_size * index;
}

/* Called from syscall or from eBPF program */
static void *array_map_lookup_elem(struct bpf_map *map, void *key)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u32 index = *(u32 *)key;

	if (unlikely(index >= array->map.max_entries))
		return NULL;

	return array->value + (u64)array->elem_size * (index & array->index_mask);
}

static int array_map_direct_value_addr(const struct bpf_map *map, u64 *imm,
				       u32 off)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);

	if (map->max_entries != 1)
		return -ENOTSUPP;
	if (off >= map->value_size)
		return -EINVAL;

	*imm = (unsigned long)array->value;
	return 0;
}

static int array_map_direct_value_meta(const struct bpf_map *map, u64 imm,
				       u32 *off)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u64 base = (unsigned long)array->value;
	u64 range = array->elem_size;

	if (map->max_entries != 1)
		return -ENOTSUPP;
	if (imm < base || imm >= base + range)
		return -ENOENT;

	*off = imm - base;
	return 0;
}

/* emit BPF instructions equivalent to C code of array_map_lookup_elem() */
static int array_map_gen_lookup(struct bpf_map *map, struct bpf_insn *insn_buf)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	struct bpf_insn *insn = insn_buf;
	u32 elem_size = array->elem_size;
	const int ret = BPF_REG_0;
	const int map_ptr = BPF_REG_1;
	const int index = BPF_REG_2;

	if (map->map_flags & BPF_F_INNER_MAP)
		return -EOPNOTSUPP;

	*insn++ = BPF_ALU64_IMM(BPF_ADD, map_ptr, offsetof(struct bpf_array, value));
	*insn++ = BPF_LDX_MEM(BPF_W, ret, index, 0);
	if (!map->bypass_spec_v1) {
		*insn++ = BPF_JMP_IMM(BPF_JGE, ret, map->max_entries, 4);
		*insn++ = BPF_ALU32_IMM(BPF_AND, ret, array->index_mask);
	} else {
		*insn++ = BPF_JMP_IMM(BPF_JGE, ret, map->max_entries, 3);
	}

	if (is_power_of_2(elem_size)) {
		*insn++ = BPF_ALU64_IMM(BPF_LSH, ret, ilog2(elem_size));
	} else {
		*insn++ = BPF_ALU64_IMM(BPF_MUL, ret, elem_size);
	}
	*insn++ = BPF_ALU64_REG(BPF_ADD, ret, map_ptr);
	*insn++ = BPF_JMP_IMM(BPF_JA, 0, 0, 1);
	*insn++ = BPF_MOV64_IMM(ret, 0);
	return insn - insn_buf;
}

/* Called from eBPF program */
static void *percpu_array_map_lookup_elem(struct bpf_map *map, void *key)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u32 index = *(u32 *)key;

	if (unlikely(index >= array->map.max_entries))
		return NULL;

	return this_cpu_ptr(array->pptrs[index & array->index_mask]);
}

/* emit BPF instructions equivalent to C code of percpu_array_map_lookup_elem() */
static int percpu_array_map_gen_lookup(struct bpf_map *map, struct bpf_insn *insn_buf)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	struct bpf_insn *insn = insn_buf;

	if (!bpf_jit_supports_percpu_insn())
		return -EOPNOTSUPP;

	if (map->map_flags & BPF_F_INNER_MAP)
		return -EOPNOTSUPP;

	BUILD_BUG_ON(offsetof(struct bpf_array, map) != 0);
	*insn++ = BPF_ALU64_IMM(BPF_ADD, BPF_REG_1, offsetof(struct bpf_array, pptrs));

	*insn++ = BPF_LDX_MEM(BPF_W, BPF_REG_0, BPF_REG_2, 0);
	if (!map->bypass_spec_v1) {
		*insn++ = BPF_JMP_IMM(BPF_JGE, BPF_REG_0, map->max_entries, 6);
		*insn++ = BPF_ALU32_IMM(BPF_AND, BPF_REG_0, array->index_mask);
	} else {
		*insn++ = BPF_JMP_IMM(BPF_JGE, BPF_REG_0, map->max_entries, 5);
	}

	*insn++ = BPF_ALU64_IMM(BPF_LSH, BPF_REG_0, 3);
	*insn++ = BPF_ALU64_REG(BPF_ADD, BPF_REG_0, BPF_REG_1);
	*insn++ = BPF_LDX_MEM(BPF_DW, BPF_REG_0, BPF_REG_0, 0);
	*insn++ = BPF_MOV64_PERCPU_REG(BPF_REG_0, BPF_REG_0);
	*insn++ = BPF_JMP_IMM(BPF_JA, 0, 0, 1);
	*insn++ = BPF_MOV64_IMM(BPF_REG_0, 0);
	return insn - insn_buf;
}

static void *percpu_array_map_lookup_percpu_elem(struct bpf_map *map, void *key, u32 cpu)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u32 index = *(u32 *)key;

	if (cpu >= nr_cpu_ids)
		return NULL;

	if (unlikely(index >= array->map.max_entries))
		return NULL;

	return per_cpu_ptr(array->pptrs[index & array->index_mask], cpu);
}

int bpf_percpu_array_copy(struct bpf_map *map, void *key, void *value)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u32 index = *(u32 *)key;
	void __percpu *pptr;
	int cpu, off = 0;
	u32 size;

	if (unlikely(index >= array->map.max_entries))
		return -ENOENT;

	/* per_cpu areas are zero-filled and bpf programs can only
	 * access 'value_size' of them, so copying rounded areas
	 * will not leak any kernel data
	 */
	size = array->elem_size;
	rcu_read_lock();
	pptr = array->pptrs[index & array->index_mask];
	for_each_possible_cpu(cpu) {
		copy_map_value_long(map, value + off, per_cpu_ptr(pptr, cpu));
		check_and_init_map_value(map, value + off);
		off += size;
	}
	rcu_read_unlock();
	return 0;
}

/* Called from syscall */
static int array_map_get_next_key(struct bpf_map *map, void *key, void *next_key)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u32 index = key ? *(u32 *)key : U32_MAX;
	u32 *next = (u32 *)next_key;

	if (index >= array->map.max_entries) {
		*next = 0;
		return 0;
	}

	if (index == array->map.max_entries - 1)
		return -ENOENT;

	*next = index + 1;
	return 0;
}

/* Called from syscall or from eBPF program */
static long array_map_update_elem(struct bpf_map *map, void *key, void *value,
				  u64 map_flags)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u32 index = *(u32 *)key;
	char *val;

	if (unlikely((map_flags & ~BPF_F_LOCK) > BPF_EXIST))
		/* unknown flags */
		return -EINVAL;

	if (unlikely(index >= array->map.max_entries))
		/* all elements were pre-allocated, cannot insert a new one */
		return -E2BIG;

	if (unlikely(map_flags & BPF_NOEXIST))
		/* all elements already exist */
		return -EEXIST;

	if (unlikely((map_flags & BPF_F_LOCK) &&
		     !btf_record_has_field(map->record, BPF_SPIN_LOCK)))
		return -EINVAL;

	if (array->map.map_type == BPF_MAP_TYPE_PERCPU_ARRAY) {
		val = this_cpu_ptr(array->pptrs[index & array->index_mask]);
		copy_map_value(map, val, value);
		bpf_obj_free_fields(array->map.record, val);
	} else {
		val = array->value +
			(u64)array->elem_size * (index & array->index_mask);
		if (map_flags & BPF_F_LOCK)
			copy_map_value_locked(map, val, value, false);
		else
			copy_map_value(map, val, value);
		bpf_obj_free_fields(array->map.record, val);
	}
	return 0;
}

int bpf_percpu_array_update(struct bpf_map *map, void *key, void *value,
			    u64 map_flags)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u32 index = *(u32 *)key;
	void __percpu *pptr;
	int cpu, off = 0;
	u32 size;

	if (unlikely(map_flags > BPF_EXIST))
		/* unknown flags */
		return -EINVAL;

	if (unlikely(index >= array->map.max_entries))
		/* all elements were pre-allocated, cannot insert a new one */
		return -E2BIG;

	if (unlikely(map_flags == BPF_NOEXIST))
		/* all elements already exist */
		return -EEXIST;

	/* the user space will provide round_up(value_size, 8) bytes that
	 * will be copied into per-cpu area. bpf programs can only access
	 * value_size of it. During lookup the same extra bytes will be
	 * returned or zeros which were zero-filled by percpu_alloc,
	 * so no kernel data leaks possible
	 */
	size = array->elem_size;
	rcu_read_lock();
	pptr = array->pptrs[index & array->index_mask];
	for_each_possible_cpu(cpu) {
		copy_map_value_long(map, per_cpu_ptr(pptr, cpu), value + off);
		bpf_obj_free_fields(array->map.record, per_cpu_ptr(pptr, cpu));
		off += size;
	}
	rcu_read_unlock();
	return 0;
}

/* Called from syscall or from eBPF program */
static long array_map_delete_elem(struct bpf_map *map, void *key)
{
	return -EINVAL;
}

static void *array_map_vmalloc_addr(struct bpf_array *array)
{
	return (void *)round_down((unsigned long)array, PAGE_SIZE);
}

static void array_map_free_timers_wq(struct bpf_map *map)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	int i;

	/* We don't reset or free fields other than timer and workqueue
	 * on uref dropping to zero.
	 */
	if (btf_record_has_field(map->record, BPF_TIMER | BPF_WORKQUEUE)) {
		for (i = 0; i < array->map.max_entries; i++) {
			if (btf_record_has_field(map->record, BPF_TIMER))
				bpf_obj_free_timer(map->record, array_map_elem_ptr(array, i));
			if (btf_record_has_field(map->record, BPF_WORKQUEUE))
				bpf_obj_free_workqueue(map->record, array_map_elem_ptr(array, i));
		}
	}
}

/* Called when map->refcnt goes to zero, either from workqueue or from syscall */
static void array_map_free(struct bpf_map *map)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	int i;

	if (!IS_ERR_OR_NULL(map->record)) {
		if (array->map.map_type == BPF_MAP_TYPE_PERCPU_ARRAY) {
			for (i = 0; i < array->map.max_entries; i++) {
				void __percpu *pptr = array->pptrs[i & array->index_mask];
				int cpu;

				for_each_possible_cpu(cpu) {
					bpf_obj_free_fields(map->record, per_cpu_ptr(pptr, cpu));
					cond_resched();
				}
			}
		} else {
			for (i = 0; i < array->map.max_entries; i++)
				bpf_obj_free_fields(map->record, array_map_elem_ptr(array, i));
		}
	}

	if (array->map.map_type == BPF_MAP_TYPE_PERCPU_ARRAY)
		bpf_array_free_percpu(array);

	if (array->map.map_flags & BPF_F_MMAPABLE)
		bpf_map_area_free(array_map_vmalloc_addr(array));
	else
		bpf_map_area_free(array);
}

static void array_map_seq_show_elem(struct bpf_map *map, void *key,
				    struct seq_file *m)
{
	void *value;

	rcu_read_lock();

	value = array_map_lookup_elem(map, key);
	if (!value) {
		rcu_read_unlock();
		return;
	}

	if (map->btf_key_type_id)
		seq_printf(m, "%u: ", *(u32 *)key);
	btf_type_seq_show(map->btf, map->btf_value_type_id, value, m);
	seq_putc(m, '\n');

	rcu_read_unlock();
}

static void percpu_array_map_seq_show_elem(struct bpf_map *map, void *key,
					   struct seq_file *m)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u32 index = *(u32 *)key;
	void __percpu *pptr;
	int cpu;

	rcu_read_lock();

	seq_printf(m, "%u: {\n", *(u32 *)key);
	pptr = array->pptrs[index & array->index_mask];
	for_each_possible_cpu(cpu) {
		seq_printf(m, "\tcpu%d: ", cpu);
		btf_type_seq_show(map->btf, map->btf_value_type_id,
				  per_cpu_ptr(pptr, cpu), m);
		seq_putc(m, '\n');
	}
	seq_puts(m, "}\n");

	rcu_read_unlock();
}

static int array_map_check_btf(const struct bpf_map *map,
			       const struct btf *btf,
			       const struct btf_type *key_type,
			       const struct btf_type *value_type)
{
	/* One exception for keyless BTF: .bss/.data/.rodata map */
	if (btf_type_is_void(key_type)) {
		if (map->map_type != BPF_MAP_TYPE_ARRAY ||
		    map->max_entries != 1)
			return -EINVAL;

		if (BTF_INFO_KIND(value_type->info) != BTF_KIND_DATASEC)
			return -EINVAL;

		return 0;
	}

	/*
	 * Bpf array can only take a u32 key. This check makes sure
	 * that the btf matches the attr used during map_create.
	 */
	if (!btf_type_is_i32(key_type))
		return -EINVAL;

	return 0;
}

static int array_map_mmap(struct bpf_map *map, struct vm_area_struct *vma)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	pgoff_t pgoff = PAGE_ALIGN(sizeof(*array)) >> PAGE_SHIFT;

	if (!(map->map_flags & BPF_F_MMAPABLE))
		return -EINVAL;

	if (vma->vm_pgoff * PAGE_SIZE + (vma->vm_end - vma->vm_start) >
	    PAGE_ALIGN((u64)array->map.max_entries * array->elem_size))
		return -EINVAL;

	return remap_vmalloc_range(vma, array_map_vmalloc_addr(array),
				   vma->vm_pgoff + pgoff);
}

static bool array_map_meta_equal(const struct bpf_map *meta0,
				 const struct bpf_map *meta1)
{
	if (!bpf_map_meta_equal(meta0, meta1))
		return false;
	return meta0->map_flags & BPF_F_INNER_MAP ? true :
	       meta0->max_entries == meta1->max_entries;
}

struct bpf_iter_seq_array_map_info {
	struct bpf_map *map;
	void *percpu_value_buf;
	u32 index;
};

static void *bpf_array_map_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct bpf_iter_seq_array_map_info *info = seq->private;
	struct bpf_map *map = info->map;
	struct bpf_array *array;
	u32 index;

	if (info->index >= map->max_entries)
		return NULL;

	if (*pos == 0)
		++*pos;
	array = container_of(map, struct bpf_array, map);
	index = info->index & array->index_mask;
	if (info->percpu_value_buf)
		return (void *)(uintptr_t)array->pptrs[index];
	return array_map_elem_ptr(array, index);
}

static void *bpf_array_map_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct bpf_iter_seq_array_map_info *info = seq->private;
	struct bpf_map *map = info->map;
	struct bpf_array *array;
	u32 index;

	++*pos;
	++info->index;
	if (info->index >= map->max_entries)
		return NULL;

	array = container_of(map, struct bpf_array, map);
	index = info->index & array->index_mask;
	if (info->percpu_value_buf)
		return (void *)(uintptr_t)array->pptrs[index];
	return array_map_elem_ptr(array, index);
}

static int __bpf_array_map_seq_show(struct seq_file *seq, void *v)
{
	struct bpf_iter_seq_array_map_info *info = seq->private;
	struct bpf_iter__bpf_map_elem ctx = {};
	struct bpf_map *map = info->map;
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	struct bpf_iter_meta meta;
	struct bpf_prog *prog;
	int off = 0, cpu = 0;
	void __percpu *pptr;
	u32 size;

	meta.seq = seq;
	prog = bpf_iter_get_info(&meta, v == NULL);
	if (!prog)
		return 0;

	ctx.meta = &meta;
	ctx.map = info->map;
	if (v) {
		ctx.key = &info->index;

		if (!info->percpu_value_buf) {
			ctx.value = v;
		} else {
			pptr = (void __percpu *)(uintptr_t)v;
			size = array->elem_size;
			for_each_possible_cpu(cpu) {
				copy_map_value_long(map, info->percpu_value_buf + off,
						    per_cpu_ptr(pptr, cpu));
				check_and_init_map_value(map, info->percpu_value_buf + off);
				off += size;
			}
			ctx.value = info->percpu_value_buf;
		}
	}

	return bpf_iter_run_prog(prog, &ctx);
}

static int bpf_array_map_seq_show(struct seq_file *seq, void *v)
{
	return __bpf_array_map_seq_show(seq, v);
}

static void bpf_array_map_seq_stop(struct seq_file *seq, void *v)
{
	if (!v)
		(void)__bpf_array_map_seq_show(seq, NULL);
}

static int bpf_iter_init_array_map(void *priv_data,
				   struct bpf_iter_aux_info *aux)
{
	struct bpf_iter_seq_array_map_info *seq_info = priv_data;
	struct bpf_map *map = aux->map;
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	void *value_buf;
	u32 buf_size;

	if (map->map_type == BPF_MAP_TYPE_PERCPU_ARRAY) {
		buf_size = array->elem_size * num_possible_cpus();
		value_buf = kmalloc(buf_size, GFP_USER | __GFP_NOWARN);
		if (!value_buf)
			return -ENOMEM;

		seq_info->percpu_value_buf = value_buf;
	}

	/* bpf_iter_attach_map() acquires a map uref, and the uref may be
	 * released before or in the middle of iterating map elements, so
	 * acquire an extra map uref for iterator.
	 */
	bpf_map_inc_with_uref(map);
	seq_info->map = map;
	return 0;
}

static void bpf_iter_fini_array_map(void *priv_data)
{
	struct bpf_iter_seq_array_map_info *seq_info = priv_data;

	bpf_map_put_with_uref(seq_info->map);
	kfree(seq_info->percpu_value_buf);
}

static const struct seq_operations bpf_array_map_seq_ops = {
	.start	= bpf_array_map_seq_start,
	.next	= bpf_array_map_seq_next,
	.stop	= bpf_array_map_seq_stop,
	.show	= bpf_array_map_seq_show,
};

static const struct bpf_iter_seq_info iter_seq_info = {
	.seq_ops		= &bpf_array_map_seq_ops,
	.init_seq_private	= bpf_iter_init_array_map,
	.fini_seq_private	= bpf_iter_fini_array_map,
	.seq_priv_size		= sizeof(struct bpf_iter_seq_array_map_info),
};

static long bpf_for_each_array_elem(struct bpf_map *map, bpf_callback_t callback_fn,
				    void *callback_ctx, u64 flags)
{
	u32 i, key, num_elems = 0;
	struct bpf_array *array;
	bool is_percpu;
	u64 ret = 0;
	void *val;

	cant_migrate();

	if (flags != 0)
		return -EINVAL;

	is_percpu = map->map_type == BPF_MAP_TYPE_PERCPU_ARRAY;
	array = container_of(map, struct bpf_array, map);
	for (i = 0; i < map->max_entries; i++) {
		if (is_percpu)
			val = this_cpu_ptr(array->pptrs[i]);
		else
			val = array_map_elem_ptr(array, i);
		num_elems++;
		key = i;
		ret = callback_fn((u64)(long)map, (u64)(long)&key,
				  (u64)(long)val, (u64)(long)callback_ctx, 0);
		/* return value: 0 - continue, 1 - stop and return */
		if (ret)
			break;
	}

	return num_elems;
}

static u64 array_map_mem_usage(const struct bpf_map *map)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	bool percpu = map->map_type == BPF_MAP_TYPE_PERCPU_ARRAY;
	u32 elem_size = array->elem_size;
	u64 entries = map->max_entries;
	u64 usage = sizeof(*array);

	if (percpu) {
		usage += entries * sizeof(void *);
		usage += entries * elem_size * num_possible_cpus();
	} else {
		if (map->map_flags & BPF_F_MMAPABLE) {
			usage = PAGE_ALIGN(usage);
			usage += PAGE_ALIGN(entries * elem_size);
		} else {
			usage += entries * elem_size;
		}
	}
	return usage;
}

BTF_ID_LIST_SINGLE(array_map_btf_ids, struct, bpf_array)
const struct bpf_map_ops array_map_ops = {
	.map_meta_equal = array_map_meta_equal,
	.map_alloc_check = array_map_alloc_check,
	.map_alloc = array_map_alloc,
	.map_free = array_map_free,
	.map_get_next_key = array_map_get_next_key,
	.map_release_uref = array_map_free_timers_wq,
	.map_lookup_elem = array_map_lookup_elem,
	.map_update_elem = array_map_update_elem,
	.map_delete_elem = array_map_delete_elem,
	.map_gen_lookup = array_map_gen_lookup,
	.map_direct_value_addr = array_map_direct_value_addr,
	.map_direct_value_meta = array_map_direct_value_meta,
	.map_mmap = array_map_mmap,
	.map_seq_show_elem = array_map_seq_show_elem,
	.map_check_btf = array_map_check_btf,
	.map_lookup_batch = generic_map_lookup_batch,
	.map_update_batch = generic_map_update_batch,
	.map_set_for_each_callback_args = map_set_for_each_callback_args,
	.map_for_each_callback = bpf_for_each_array_elem,
	.map_mem_usage = array_map_mem_usage,
	.map_btf_id = &array_map_btf_ids[0],
	.iter_seq_info = &iter_seq_info,
};

const struct bpf_map_ops percpu_array_map_ops = {
	.map_meta_equal = bpf_map_meta_equal,
	.map_alloc_check = array_map_alloc_check,
	.map_alloc = array_map_alloc,
	.map_free = array_map_free,
	.map_get_next_key = array_map_get_next_key,
	.map_lookup_elem = percpu_array_map_lookup_elem,
	.map_gen_lookup = percpu_array_map_gen_lookup,
	.map_update_elem = array_map_update_elem,
	.map_delete_elem = array_map_delete_elem,
	.map_lookup_percpu_elem = percpu_array_map_lookup_percpu_elem,
	.map_seq_show_elem = percpu_array_map_seq_show_elem,
	.map_check_btf = array_map_check_btf,
	.map_lookup_batch = generic_map_lookup_batch,
	.map_update_batch = generic_map_update_batch,
	.map_set_for_each_callback_args = map_set_for_each_callback_args,
	.map_for_each_callback = bpf_for_each_array_elem,
	.map_mem_usage = array_map_mem_usage,
	.map_btf_id = &array_map_btf_ids[0],
	.iter_seq_info = &iter_seq_info,
};

static int fd_array_map_alloc_check(union bpf_attr *attr)
{
	/* only file descriptors can be stored in this type of map */
	if (attr->value_size != sizeof(u32))
		return -EINVAL;
	/* Program read-only/write-only not supported for special maps yet. */
	if (attr->map_flags & (BPF_F_RDONLY_PROG | BPF_F_WRONLY_PROG))
		return -EINVAL;
	return array_map_alloc_check(attr);
}

static void fd_array_map_free(struct bpf_map *map)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	int i;

	/* make sure it's empty */
	for (i = 0; i < array->map.max_entries; i++)
		BUG_ON(array->ptrs[i] != NULL);

	bpf_map_area_free(array);
}

static void *fd_array_map_lookup_elem(struct bpf_map *map, void *key)
{
	return ERR_PTR(-EOPNOTSUPP);
}

/* only called from syscall */
int bpf_fd_array_map_lookup_elem(struct bpf_map *map, void *key, u32 *value)
{
	void **elem, *ptr;
	int ret =  0;

	if (!map->ops->map_fd_sys_lookup_elem)
		return -ENOTSUPP;

	rcu_read_lock();
	elem = array_map_lookup_elem(map, key);
	if (elem && (ptr = READ_ONCE(*elem)))
		*value = map->ops->map_fd_sys_lookup_elem(ptr);
	else
		ret = -ENOENT;
	rcu_read_unlock();

	return ret;
}

/* only called from syscall */
int bpf_fd_array_map_update_elem(struct bpf_map *map, struct file *map_file,
				 void *key, void *value, u64 map_flags)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	void *new_ptr, *old_ptr;
	u32 index = *(u32 *)key, ufd;

	if (map_flags != BPF_ANY)
		return -EINVAL;

	if (index >= array->map.max_entries)
		return -E2BIG;

	ufd = *(u32 *)value;
	new_ptr = map->ops->map_fd_get_ptr(map, map_file, ufd);
	if (IS_ERR(new_ptr))
		return PTR_ERR(new_ptr);

	if (map->ops->map_poke_run) {
		mutex_lock(&array->aux->poke_mutex);
		old_ptr = xchg(array->ptrs + index, new_ptr);
		map->ops->map_poke_run(map, index, old_ptr, new_ptr);
		mutex_unlock(&array->aux->poke_mutex);
	} else {
		old_ptr = xchg(array->ptrs + index, new_ptr);
	}

	if (old_ptr)
		map->ops->map_fd_put_ptr(map, old_ptr, true);
	return 0;
}

static long __fd_array_map_delete_elem(struct bpf_map *map, void *key, bool need_defer)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	void *old_ptr;
	u32 index = *(u32 *)key;

	if (index >= array->map.max_entries)
		return -E2BIG;

	if (map->ops->map_poke_run) {
		mutex_lock(&array->aux->poke_mutex);
		old_ptr = xchg(array->ptrs + index, NULL);
		map->ops->map_poke_run(map, index, old_ptr, NULL);
		mutex_unlock(&array->aux->poke_mutex);
	} else {
		old_ptr = xchg(array->ptrs + index, NULL);
	}

	if (old_ptr) {
		map->ops->map_fd_put_ptr(map, old_ptr, need_defer);
		return 0;
	} else {
		return -ENOENT;
	}
}

static long fd_array_map_delete_elem(struct bpf_map *map, void *key)
{
	return __fd_array_map_delete_elem(map, key, true);
}

static void *prog_fd_array_get_ptr(struct bpf_map *map,
				   struct file *map_file, int fd)
{
	struct bpf_prog *prog = bpf_prog_get(fd);
	bool is_extended;

	if (IS_ERR(prog))
		return prog;

	if (prog->type == BPF_PROG_TYPE_EXT ||
	    !bpf_prog_map_compatible(map, prog)) {
		bpf_prog_put(prog);
		return ERR_PTR(-EINVAL);
	}

	mutex_lock(&prog->aux->ext_mutex);
	is_extended = prog->aux->is_extended;
	if (!is_extended)
		prog->aux->prog_array_member_cnt++;
	mutex_unlock(&prog->aux->ext_mutex);
	if (is_extended) {
		/* Extended prog can not be tail callee. It's to prevent a
		 * potential infinite loop like:
		 * tail callee prog entry -> tail callee prog subprog ->
		 * freplace prog entry --tailcall-> tail callee prog entry.
		 */
		bpf_prog_put(prog);
		return ERR_PTR(-EBUSY);
	}

	return prog;
}

static void prog_fd_array_put_ptr(struct bpf_map *map, void *ptr, bool need_defer)
{
	struct bpf_prog *prog = ptr;

	mutex_lock(&prog->aux->ext_mutex);
	prog->aux->prog_array_member_cnt--;
	mutex_unlock(&prog->aux->ext_mutex);
	/* bpf_prog is freed after one RCU or tasks trace grace period */
	bpf_prog_put(prog);
}

static u32 prog_fd_array_sys_lookup_elem(void *ptr)
{
	return ((struct bpf_prog *)ptr)->aux->id;
}

/* decrement refcnt of all bpf_progs that are stored in this map */
static void bpf_fd_array_map_clear(struct bpf_map *map, bool need_defer)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	int i;

	for (i = 0; i < array->map.max_entries; i++)
		__fd_array_map_delete_elem(map, &i, need_defer);
}

static void prog_array_map_seq_show_elem(struct bpf_map *map, void *key,
					 struct seq_file *m)
{
	void **elem, *ptr;
	u32 prog_id;

	rcu_read_lock();

	elem = array_map_lookup_elem(map, key);
	if (elem) {
		ptr = READ_ONCE(*elem);
		if (ptr) {
			seq_printf(m, "%u: ", *(u32 *)key);
			prog_id = prog_fd_array_sys_lookup_elem(ptr);
			btf_type_seq_show(map->btf, map->btf_value_type_id,
					  &prog_id, m);
			seq_putc(m, '\n');
		}
	}

	rcu_read_unlock();
}

struct prog_poke_elem {
	struct list_head list;
	struct bpf_prog_aux *aux;
};

static int prog_array_map_poke_track(struct bpf_map *map,
				     struct bpf_prog_aux *prog_aux)
{
	struct prog_poke_elem *elem;
	struct bpf_array_aux *aux;
	int ret = 0;

	aux = container_of(map, struct bpf_array, map)->aux;
	mutex_lock(&aux->poke_mutex);
	list_for_each_entry(elem, &aux->poke_progs, list) {
		if (elem->aux == prog_aux)
			goto out;
	}

	elem = kmalloc(sizeof(*elem), GFP_KERNEL);
	if (!elem) {
		ret = -ENOMEM;
		goto out;
	}

	INIT_LIST_HEAD(&elem->list);
	/* We must track the program's aux info at this point in time
	 * since the program pointer itself may not be stable yet, see
	 * also comment in prog_array_map_poke_run().
	 */
	elem->aux = prog_aux;

	list_add_tail(&elem->list, &aux->poke_progs);
out:
	mutex_unlock(&aux->poke_mutex);
	return ret;
}

static void prog_array_map_poke_untrack(struct bpf_map *map,
					struct bpf_prog_aux *prog_aux)
{
	struct prog_poke_elem *elem, *tmp;
	struct bpf_array_aux *aux;

	aux = container_of(map, struct bpf_array, map)->aux;
	mutex_lock(&aux->poke_mutex);
	list_for_each_entry_safe(elem, tmp, &aux->poke_progs, list) {
		if (elem->aux == prog_aux) {
			list_del_init(&elem->list);
			kfree(elem);
			break;
		}
	}
	mutex_unlock(&aux->poke_mutex);
}

void __weak bpf_arch_poke_desc_update(struct bpf_jit_poke_descriptor *poke,
				      struct bpf_prog *new, struct bpf_prog *old)
{
	WARN_ON_ONCE(1);
}

static void prog_array_map_poke_run(struct bpf_map *map, u32 key,
				    struct bpf_prog *old,
				    struct bpf_prog *new)
{
	struct prog_poke_elem *elem;
	struct bpf_array_aux *aux;

	aux = container_of(map, struct bpf_array, map)->aux;
	WARN_ON_ONCE(!mutex_is_locked(&aux->poke_mutex));

	list_for_each_entry(elem, &aux->poke_progs, list) {
		struct bpf_jit_poke_descriptor *poke;
		int i;

		for (i = 0; i < elem->aux->size_poke_tab; i++) {
			poke = &elem->aux->poke_tab[i];

			/* Few things to be aware of:
			 *
			 * 1) We can only ever access aux in this context, but
			 *    not aux->prog since it might not be stable yet and
			 *    there could be danger of use after free otherwise.
			 * 2) Initially when we start tracking aux, the program
			 *    is not JITed yet and also does not have a kallsyms
			 *    entry. We skip these as poke->tailcall_target_stable
			 *    is not active yet. The JIT will do the final fixup
			 *    before setting it stable. The various
			 *    poke->tailcall_target_stable are successively
			 *    activated, so tail call updates can arrive from here
			 *    while JIT is still finishing its final fixup for
			 *    non-activated poke entries.
			 * 3) Also programs reaching refcount of zero while patching
			 *    is in progress is okay since we're protected under
			 *    poke_mutex and untrack the programs before the JIT
			 *    buffer is freed.
			 */
			if (!READ_ONCE(poke->tailcall_target_stable))
				continue;
			if (poke->reason != BPF_POKE_REASON_TAIL_CALL)
				continue;
			if (poke->tail_call.map != map ||
			    poke->tail_call.key != key)
				continue;

			bpf_arch_poke_desc_update(poke, new, old);
		}
	}
}

static void prog_array_map_clear_deferred(struct work_struct *work)
{
	struct bpf_map *map = container_of(work, struct bpf_array_aux,
					   work)->map;
	bpf_fd_array_map_clear(map, true);
	bpf_map_put(map);
}

static void prog_array_map_clear(struct bpf_map *map)
{
	struct bpf_array_aux *aux = container_of(map, struct bpf_array,
						 map)->aux;
	bpf_map_inc(map);
	schedule_work(&aux->work);
}

static struct bpf_map *prog_array_map_alloc(union bpf_attr *attr)
{
	struct bpf_array_aux *aux;
	struct bpf_map *map;

	aux = kzalloc(sizeof(*aux), GFP_KERNEL_ACCOUNT);
	if (!aux)
		return ERR_PTR(-ENOMEM);

	INIT_WORK(&aux->work, prog_array_map_clear_deferred);
	INIT_LIST_HEAD(&aux->poke_progs);
	mutex_init(&aux->poke_mutex);

	map = array_map_alloc(attr);
	if (IS_ERR(map)) {
		kfree(aux);
		return map;
	}

	container_of(map, struct bpf_array, map)->aux = aux;
	aux->map = map;

	return map;
}

static void prog_array_map_free(struct bpf_map *map)
{
	struct prog_poke_elem *elem, *tmp;
	struct bpf_array_aux *aux;

	aux = container_of(map, struct bpf_array, map)->aux;
	list_for_each_entry_safe(elem, tmp, &aux->poke_progs, list) {
		list_del_init(&elem->list);
		kfree(elem);
	}
	kfree(aux);
	fd_array_map_free(map);
}

/* prog_array->aux->{type,jited} is a runtime binding.
 * Doing static check alone in the verifier is not enough.
 * Thus, prog_array_map cannot be used as an inner_map
 * and map_meta_equal is not implemented.
 */
const struct bpf_map_ops prog_array_map_ops = {
	.map_alloc_check = fd_array_map_alloc_check,
	.map_alloc = prog_array_map_alloc,
	.map_free = prog_array_map_free,
	.map_poke_track = prog_array_map_poke_track,
	.map_poke_untrack = prog_array_map_poke_untrack,
	.map_poke_run = prog_array_map_poke_run,
	.map_get_next_key = array_map_get_next_key,
	.map_lookup_elem = fd_array_map_lookup_elem,
	.map_delete_elem = fd_array_map_delete_elem,
	.map_fd_get_ptr = prog_fd_array_get_ptr,
	.map_fd_put_ptr = prog_fd_array_put_ptr,
	.map_fd_sys_lookup_elem = prog_fd_array_sys_lookup_elem,
	.map_release_uref = prog_array_map_clear,
	.map_seq_show_elem = prog_array_map_seq_show_elem,
	.map_mem_usage = array_map_mem_usage,
	.map_btf_id = &array_map_btf_ids[0],
};

static struct bpf_event_entry *bpf_event_entry_gen(struct file *perf_file,
						   struct file *map_file)
{
	struct bpf_event_entry *ee;

	ee = kzalloc(sizeof(*ee), GFP_KERNEL);
	if (ee) {
		ee->event = perf_file->private_data;
		ee->perf_file = perf_file;
		ee->map_file = map_file;
	}

	return ee;
}

static void __bpf_event_entry_free(struct rcu_head *rcu)
{
	struct bpf_event_entry *ee;

	ee = container_of(rcu, struct bpf_event_entry, rcu);
	fput(ee->perf_file);
	kfree(ee);
}

static void bpf_event_entry_free_rcu(struct bpf_event_entry *ee)
{
	call_rcu(&ee->rcu, __bpf_event_entry_free);
}

static void *perf_event_fd_array_get_ptr(struct bpf_map *map,
					 struct file *map_file, int fd)
{
	struct bpf_event_entry *ee;
	struct perf_event *event;
	struct file *perf_file;
	u64 value;

	perf_file = perf_event_get(fd);
	if (IS_ERR(perf_file))
		return perf_file;

	ee = ERR_PTR(-EOPNOTSUPP);
	event = perf_file->private_data;
	if (perf_event_read_local(event, &value, NULL, NULL) == -EOPNOTSUPP)
		goto err_out;

	ee = bpf_event_entry_gen(perf_file, map_file);
	if (ee)
		return ee;
	ee = ERR_PTR(-ENOMEM);
err_out:
	fput(perf_file);
	return ee;
}

static void perf_event_fd_array_put_ptr(struct bpf_map *map, void *ptr, bool need_defer)
{
	/* bpf_perf_event is freed after one RCU grace period */
	bpf_event_entry_free_rcu(ptr);
}

static void perf_event_fd_array_release(struct bpf_map *map,
					struct file *map_file)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	struct bpf_event_entry *ee;
	int i;

	if (map->map_flags & BPF_F_PRESERVE_ELEMS)
		return;

	rcu_read_lock();
	for (i = 0; i < array->map.max_entries; i++) {
		ee = READ_ONCE(array->ptrs[i]);
		if (ee && ee->map_file == map_file)
			__fd_array_map_delete_elem(map, &i, true);
	}
	rcu_read_unlock();
}

static void perf_event_fd_array_map_free(struct bpf_map *map)
{
	if (map->map_flags & BPF_F_PRESERVE_ELEMS)
		bpf_fd_array_map_clear(map, false);
	fd_array_map_free(map);
}

const struct bpf_map_ops perf_event_array_map_ops = {
	.map_meta_equal = bpf_map_meta_equal,
	.map_alloc_check = fd_array_map_alloc_check,
	.map_alloc = array_map_alloc,
	.map_free = perf_event_fd_array_map_free,
	.map_get_next_key = array_map_get_next_key,
	.map_lookup_elem = fd_array_map_lookup_elem,
	.map_delete_elem = fd_array_map_delete_elem,
	.map_fd_get_ptr = perf_event_fd_array_get_ptr,
	.map_fd_put_ptr = perf_event_fd_array_put_ptr,
	.map_release = perf_event_fd_array_release,
	.map_check_btf = map_check_no_btf,
	.map_mem_usage = array_map_mem_usage,
	.map_btf_id = &array_map_btf_ids[0],
};

#ifdef CONFIG_CGROUPS
static void *cgroup_fd_array_get_ptr(struct bpf_map *map,
				     struct file *map_file /* not used */,
				     int fd)
{
	return cgroup_get_from_fd(fd);
}

static void cgroup_fd_array_put_ptr(struct bpf_map *map, void *ptr, bool need_defer)
{
	/* cgroup_put free cgrp after a rcu grace period */
	cgroup_put(ptr);
}

static void cgroup_fd_array_free(struct bpf_map *map)
{
	bpf_fd_array_map_clear(map, false);
	fd_array_map_free(map);
}

const struct bpf_map_ops cgroup_array_map_ops = {
	.map_meta_equal = bpf_map_meta_equal,
	.map_alloc_check = fd_array_map_alloc_check,
	.map_alloc = array_map_alloc,
	.map_free = cgroup_fd_array_free,
	.map_get_next_key = array_map_get_next_key,
	.map_lookup_elem = fd_array_map_lookup_elem,
	.map_delete_elem = fd_array_map_delete_elem,
	.map_fd_get_ptr = cgroup_fd_array_get_ptr,
	.map_fd_put_ptr = cgroup_fd_array_put_ptr,
	.map_check_btf = map_check_no_btf,
	.map_mem_usage = array_map_mem_usage,
	.map_btf_id = &array_map_btf_ids[0],
};
#endif

static struct bpf_map *array_of_map_alloc(union bpf_attr *attr)
{
	struct bpf_map *map, *inner_map_meta;

	inner_map_meta = bpf_map_meta_alloc(attr->inner_map_fd);
	if (IS_ERR(inner_map_meta))
		return inner_map_meta;

	map = array_map_alloc(attr);
	if (IS_ERR(map)) {
		bpf_map_meta_free(inner_map_meta);
		return map;
	}

	map->inner_map_meta = inner_map_meta;

	return map;
}

static void array_of_map_free(struct bpf_map *map)
{
	/* map->inner_map_meta is only accessed by syscall which
	 * is protected by fdget/fdput.
	 */
	bpf_map_meta_free(map->inner_map_meta);
	bpf_fd_array_map_clear(map, false);
	fd_array_map_free(map);
}

static void *array_of_map_lookup_elem(struct bpf_map *map, void *key)
{
	struct bpf_map **inner_map = array_map_lookup_elem(map, key);

	if (!inner_map)
		return NULL;

	return READ_ONCE(*inner_map);
}

static int array_of_map_gen_lookup(struct bpf_map *map,
				   struct bpf_insn *insn_buf)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u32 elem_size = array->elem_size;
	struct bpf_insn *insn = insn_buf;
	const int ret = BPF_REG_0;
	const int map_ptr = BPF_REG_1;
	const int index = BPF_REG_2;

	*insn++ = BPF_ALU64_IMM(BPF_ADD, map_ptr, offsetof(struct bpf_array, value));
	*insn++ = BPF_LDX_MEM(BPF_W, ret, index, 0);
	if (!map->bypass_spec_v1) {
		*insn++ = BPF_JMP_IMM(BPF_JGE, ret, map->max_entries, 6);
		*insn++ = BPF_ALU32_IMM(BPF_AND, ret, array->index_mask);
	} else {
		*insn++ = BPF_JMP_IMM(BPF_JGE, ret, map->max_entries, 5);
	}
	if (is_power_of_2(elem_size))
		*insn++ = BPF_ALU64_IMM(BPF_LSH, ret, ilog2(elem_size));
	else
		*insn++ = BPF_ALU64_IMM(BPF_MUL, ret, elem_size);
	*insn++ = BPF_ALU64_REG(BPF_ADD, ret, map_ptr);
	*insn++ = BPF_LDX_MEM(BPF_DW, ret, ret, 0);
	*insn++ = BPF_JMP_IMM(BPF_JEQ, ret, 0, 1);
	*insn++ = BPF_JMP_IMM(BPF_JA, 0, 0, 1);
	*insn++ = BPF_MOV64_IMM(ret, 0);

	return insn - insn_buf;
}

const struct bpf_map_ops array_of_maps_map_ops = {
	.map_alloc_check = fd_array_map_alloc_check,
	.map_alloc = array_of_map_alloc,
	.map_free = array_of_map_free,
	.map_get_next_key = array_map_get_next_key,
	.map_lookup_elem = array_of_map_lookup_elem,
	.map_delete_elem = fd_array_map_delete_elem,
	.map_fd_get_ptr = bpf_map_fd_get_ptr,
	.map_fd_put_ptr = bpf_map_fd_put_ptr,
	.map_fd_sys_lookup_elem = bpf_map_fd_sys_lookup_elem,
	.map_gen_lookup = array_of_map_gen_lookup,
	.map_lookup_batch = generic_map_lookup_batch,
	.map_update_batch = generic_map_update_batch,
	.map_check_btf = map_check_no_btf,
	.map_mem_usage = array_map_mem_usage,
	.map_btf_id = &array_map_btf_ids[0],
};
