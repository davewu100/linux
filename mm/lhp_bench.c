// SPDX-License-Identifier: GPL-2.0
/*
 * lhp_bench - microbenchmark module for the LHP allocator, with a comparison
 * against the equivalent standard-kernel allocator.
 *
 * A standalone, loadable performance tool for mm/lhp.c that drives the pool
 * only through its public exported API, so it stays decoupled from the
 * allocator internals and costs nothing when not loaded.
 *
 * Interface (created on module load, removed on unload):
 *
 *   /sys/kernel/debug/lhp_bench
 *       write "<ops> [threads] [lhp|std] [64|4k|2m]" to run the (optionally
 *       concurrent) benchmark; read to see the last run's results.
 *
 * Each thread repeatedly allocates then immediately frees one object of the
 * chosen size @ops times.  The "lhp" and "std" backends allocate a comparable
 * object so the two numbers can be compared directly:
 *
 *   64B / 4K : lhp_malloc() on a shared heap   vs  kmalloc()
 *   2M       : lhp_alloc_2m()                  vs  alloc_pages(order 9)
 *
 * Running T threads on T CPUs exercises the allocator's locking.  For the 2M
 * LHP backend, per-region locking with round-robin selection should scale
 * close to linearly when there are more regions than threads.
 */
#define pr_fmt(fmt) "lhp_bench: " fmt

#include <linux/lhp.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/sched.h>
#include <linux/err.h>

#define LHP_BENCH_MAX_THREADS	64
#define LHP_ORDER_2M		9		/* 2M / 4K */

/*
 * What to benchmark.  Each size has an LHP path and a fair standard-kernel
 * counterpart:
 *
 *   64B / 4K : lhp_malloc() on a shared heap   vs  kmalloc()
 *   2M       : lhp_alloc_2m()                  vs  alloc_pages(order 9)
 */
enum lhp_bench_backend {
	LHP_BE_LHP,	/* the LHP allocator */
	LHP_BE_STD,	/* the standard kernel allocator for the same size */
};

enum lhp_bench_size {
	LHP_SZ_64,
	LHP_SZ_4K,
	LHP_SZ_2M,
};

static const struct {
	const char	*name;
	size_t		bytes;
} lhp_bench_sizes[] = {
	[LHP_SZ_64] = { "64",  64 },
	[LHP_SZ_4K] = { "4k",  4096 },
	[LHP_SZ_2M] = { "2m",  2 * 1024 * 1024 },
};

/* Shared heap used by the 64B/4K LHP backend, set up for the duration of a run. */
static struct lhp_heap *lhp_bench_heap;

struct lhp_bench_thread {
	struct task_struct	*task;
	unsigned long		ops;		/* iterations to run */
	unsigned long		done;		/* iterations completed */
	u64			ns;		/* per-thread wall time */
	enum lhp_bench_backend	backend;
	enum lhp_bench_size	size;
	struct completion	start;
	struct completion	end;
};

struct lhp_bench_result {
	unsigned long	ops_per_thread;
	unsigned int	threads;
	unsigned long	total_ops;
	u64		wall_ns;	/* max thread wall time (parallel) */
	u64		sum_ns;		/* sum of thread times (total CPU) */
	enum lhp_bench_backend	backend;
	enum lhp_bench_size	size;
	bool		valid;
};
static struct lhp_bench_result lhp_bench_last;

static struct dentry *lhp_bench_dentry;

/*
 * Perform one alloc+free of the configured backend/size.  Returns true on
 * success, false if the allocation failed (which stops the thread's loop).
 */
static bool lhp_bench_op(enum lhp_bench_backend be, enum lhp_bench_size sz)
{
	size_t bytes = lhp_bench_sizes[sz].bytes;

	if (sz == LHP_SZ_2M) {
		/* 2M: chunk allocator vs buddy order-9. */
		if (be == LHP_BE_LHP) {
			struct page *pg = lhp_alloc_2m(GFP_KERNEL);

			if (!pg)
				return false;
			lhp_free_2m(pg);
		} else {
			struct page *pg = alloc_pages(GFP_KERNEL, LHP_ORDER_2M);

			if (!pg)
				return false;
			__free_pages(pg, LHP_ORDER_2M);
		}
	} else {
		/* 64B / 4K: heap vs kmalloc. */
		if (be == LHP_BE_LHP) {
			void *p = lhp_malloc(lhp_bench_heap, bytes, 0,
					     GFP_KERNEL);

			if (!p)
				return false;
			lhp_free(p);
		} else {
			void *p = kmalloc(bytes, GFP_KERNEL);

			if (!p)
				return false;
			kfree(p);
		}
	}
	return true;
}

static int lhp_bench_fn(void *arg)
{
	struct lhp_bench_thread *t = arg;
	unsigned long i;
	ktime_t t0, t1;

	wait_for_completion(&t->start);

	t0 = ktime_get();
	for (i = 0; i < t->ops; i++) {
		if (!lhp_bench_op(t->backend, t->size))
			break;
	}
	t1 = ktime_get();

	t->done = i;
	t->ns = ktime_to_ns(ktime_sub(t1, t0));
	complete(&t->end);

	/* Park until kthread_stop() so the task_struct stays valid. */
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ / 10);
	return 0;
}

static int lhp_bench_run(unsigned long ops_per_thread, unsigned int threads,
			 enum lhp_bench_backend backend, enum lhp_bench_size size)
{
	struct lhp_bench_thread *t;
	unsigned int i, launched = 0;
	u64 wall = 0, sum = 0;
	unsigned long total_done = 0;
	bool need_heap = (backend == LHP_BE_LHP && size != LHP_SZ_2M);

	/*
	 * The 64B/4K LHP backend allocates from a single shared heap so that
	 * concurrent threads exercise the heap lock.  A 1G-backed heap gives
	 * one large contiguous arena, plenty for the working set.
	 */
	if (need_heap) {
		lhp_bench_heap = lhp_heap_create_1g("lhp_bench", NUMA_NO_NODE,
						    GFP_KERNEL);
		if (IS_ERR(lhp_bench_heap)) {
			int ret = PTR_ERR(lhp_bench_heap);

			lhp_bench_heap = NULL;
			return ret;
		}
	}

	/*
	 * Cap at one thread per online CPU so each thread gets a distinct CPU
	 * via kthread_bind() below.  Otherwise threads would share CPUs and the
	 * reported "parallel /op" scaling number would be skewed.
	 */
	threads = min(threads, num_online_cpus());

	t = kcalloc(threads, sizeof(*t), GFP_KERNEL);
	if (!t) {
		if (need_heap) {
			lhp_heap_destroy(lhp_bench_heap);
			lhp_bench_heap = NULL;
		}
		return -ENOMEM;
	}

	for (i = 0; i < threads; i++) {
		t[i].ops = ops_per_thread;
		t[i].backend = backend;
		t[i].size = size;
		init_completion(&t[i].start);
		init_completion(&t[i].end);
		t[i].task = kthread_create(lhp_bench_fn, &t[i],
					   "lhp_bench/%u", i);
		if (IS_ERR(t[i].task))
			break;
		kthread_bind(t[i].task, i);
		wake_up_process(t[i].task);
		launched++;
	}
	if (!launched) {
		kfree(t);
		if (need_heap) {
			lhp_heap_destroy(lhp_bench_heap);
			lhp_bench_heap = NULL;
		}
		return -EAGAIN;
	}

	/* Release all threads as simultaneously as possible. */
	for (i = 0; i < launched; i++)
		complete(&t[i].start);

	for (i = 0; i < launched; i++) {
		wait_for_completion(&t[i].end);
		wall = max(wall, t[i].ns);
		sum += t[i].ns;
		total_done += t[i].done;
	}

	for (i = 0; i < launched; i++)
		kthread_stop(t[i].task);

	lhp_bench_last.ops_per_thread = ops_per_thread;
	lhp_bench_last.threads = launched;
	lhp_bench_last.total_ops = total_done;
	lhp_bench_last.wall_ns = wall;
	lhp_bench_last.sum_ns = sum;
	lhp_bench_last.backend = backend;
	lhp_bench_last.size = size;
	lhp_bench_last.valid = true;

	pr_info("%s/%s: %u thread(s) x %lu ops  %llu ns/op (parallel)  %llu ns/op (per-cpu)\n",
		backend == LHP_BE_LHP ? "lhp" : "std",
		lhp_bench_sizes[size].name, launched, ops_per_thread,
		total_done ? wall * launched / total_done : 0,
		total_done ? sum / total_done : 0);

	kfree(t);
	if (need_heap) {
		lhp_heap_destroy(lhp_bench_heap);
		lhp_bench_heap = NULL;
	}
	return 0;
}

/*
 * Command: "<ops> [threads] [lhp|std] [64|4k|2m]".
 * @what and @size default to "lhp" and "2m".
 */
static ssize_t lhp_bench_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	char buf[64], what[8] = "lhp", szname[8] = "2m";
	enum lhp_bench_backend backend;
	enum lhp_bench_size size;
	unsigned long ops;
	unsigned int threads = 1, i;
	int ret;

	if (!lhp_pool_available())
		return -ENODEV;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	ret = sscanf(buf, "%lu %u %7s %7s", &ops, &threads, what, szname);
	if (ret < 1)
		return -EINVAL;
	if (!ops || ops > (1UL << 22))
		return -EINVAL;
	if (threads < 1 || threads > LHP_BENCH_MAX_THREADS)
		return -EINVAL;

	if (!strcmp(what, "lhp"))
		backend = LHP_BE_LHP;
	else if (!strcmp(what, "std"))
		backend = LHP_BE_STD;
	else
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(lhp_bench_sizes); i++)
		if (!strcmp(szname, lhp_bench_sizes[i].name))
			break;
	if (i >= ARRAY_SIZE(lhp_bench_sizes))
		return -EINVAL;
	size = i;

	ret = lhp_bench_run(ops, threads, backend, size);
	if (ret)
		return ret;
	return count;
}

static int lhp_bench_show(struct seq_file *m, void *v)
{
	struct lhp_bench_result r = lhp_bench_last;

	if (!r.valid) {
		seq_puts(m, "no run yet; write \"<ops> [threads] [lhp|std] [64|4k|2m]\"\n");
		return 0;
	}
	seq_printf(m, "backend:        %s\n",
		   r.backend == LHP_BE_LHP ? "lhp" : "std");
	seq_printf(m, "size:           %s\n", lhp_bench_sizes[r.size].name);
	seq_printf(m, "threads:        %u\n", r.threads);
	seq_printf(m, "ops/thread:     %lu\n", r.ops_per_thread);
	seq_printf(m, "total ops done: %lu\n", r.total_ops);
	seq_printf(m, "wall time:      %llu ns\n", r.wall_ns);
	seq_printf(m, "parallel /op:   %llu ns\n",
		   r.total_ops ? r.wall_ns * r.threads / r.total_ops : 0);
	seq_printf(m, "per-cpu /op:    %llu ns\n",
		   r.total_ops ? r.sum_ns / r.total_ops : 0);
	return 0;
}

static int lhp_bench_open(struct inode *inode, struct file *file)
{
	return single_open(file, lhp_bench_show, NULL);
}

static const struct file_operations lhp_bench_fops = {
	.open = lhp_bench_open,
	.read = seq_read,
	.write = lhp_bench_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init lhp_bench_init(void)
{
	lhp_bench_dentry = debugfs_create_file("lhp_bench", 0600, NULL, NULL,
					       &lhp_bench_fops);
	pr_info("loaded; write \"<ops> [threads] [lhp|std] [64|4k|2m]\" to /sys/kernel/debug/lhp_bench\n");
	return 0;
}
module_init(lhp_bench_init);

static void __exit lhp_bench_exit(void)
{
	debugfs_remove(lhp_bench_dentry);
}
module_exit(lhp_bench_exit);

MODULE_DESCRIPTION("Microbenchmark for the LHP allocator vs standard kernel allocators");
MODULE_LICENSE("GPL");
