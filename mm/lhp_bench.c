// SPDX-License-Identifier: GPL-2.0
/*
 * lhp_bench - microbenchmark module for the LHP 2M chunk allocator.
 *
 * A standalone, loadable performance tool for mm/lhp.c.  It only touches the
 * public, exported chunk API (lhp_pool_available(), lhp_alloc_2m(),
 * lhp_free_2m()), so it stays fully decoupled from the allocator internals and
 * adds zero cost to the kernel when not loaded.
 *
 * Interface (created on module load, removed on unload):
 *
 *   /sys/kernel/debug/lhp_bench   - write "<ops_per_thread> [threads]" to run
 *                                   the (optionally concurrent) microbenchmark;
 *                                   read to see the last run's results.
 *
 * Each thread repeatedly allocates then immediately frees a 2M chunk
 * @ops_per_thread times.  Running T threads on T CPUs exercises the per-region
 * locking: with more regions than threads and round-robin region selection,
 * threads mostly hit different regions and should scale close to linearly.
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

struct lhp_bench_thread {
	struct task_struct	*task;
	unsigned long		ops;		/* iterations to run */
	unsigned long		done;		/* iterations completed */
	u64			ns;		/* per-thread wall time */
	struct completion	start;
	struct completion	end;
};

struct lhp_bench_result {
	unsigned long	ops_per_thread;
	unsigned int	threads;
	unsigned long	total_ops;
	u64		wall_ns;	/* max thread wall time (parallel) */
	u64		sum_ns;		/* sum of thread times (total CPU) */
	bool		valid;
};
static struct lhp_bench_result lhp_bench_last;

static struct dentry *lhp_bench_dentry;

static int lhp_bench_fn(void *arg)
{
	struct lhp_bench_thread *t = arg;
	unsigned long i;
	ktime_t t0, t1;

	wait_for_completion(&t->start);

	t0 = ktime_get();
	for (i = 0; i < t->ops; i++) {
		struct page *pg = lhp_alloc_2m(GFP_KERNEL);

		if (!pg)
			break;
		lhp_free_2m(pg);
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

static int lhp_bench_run(unsigned long ops_per_thread, unsigned int threads)
{
	struct lhp_bench_thread *t;
	unsigned int i, launched = 0;
	u64 wall = 0, sum = 0;
	unsigned long total_done = 0;

	/*
	 * Cap at one thread per online CPU so each thread gets a distinct CPU
	 * via kthread_bind() below.  Otherwise threads would share CPUs and the
	 * reported "parallel /op" scaling number would be skewed.
	 */
	threads = min(threads, num_online_cpus());

	t = kcalloc(threads, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;

	for (i = 0; i < threads; i++) {
		t[i].ops = ops_per_thread;
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
	lhp_bench_last.valid = true;

	pr_info("%u thread(s) x %lu ops  wall=%llu ns  %llu ns/op (parallel)  %llu ns/op (per-cpu)\n",
		launched, ops_per_thread, wall,
		total_done ? wall * launched / total_done : 0,
		total_done ? sum / total_done : 0);

	kfree(t);
	return 0;
}

static ssize_t lhp_bench_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	char buf[64];
	unsigned long ops;
	unsigned int threads = 1;
	int ret;

	if (!lhp_pool_available())
		return -ENODEV;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	ret = sscanf(buf, "%lu %u", &ops, &threads);
	if (ret < 1)
		return -EINVAL;
	if (!ops || ops > (1UL << 22))
		return -EINVAL;
	if (threads < 1 || threads > LHP_BENCH_MAX_THREADS)
		return -EINVAL;

	ret = lhp_bench_run(ops, threads);
	if (ret)
		return ret;
	return count;
}

static int lhp_bench_show(struct seq_file *m, void *v)
{
	struct lhp_bench_result r = lhp_bench_last;

	if (!r.valid) {
		seq_puts(m, "no run yet; write \"<ops_per_thread> [threads]\"\n");
		return 0;
	}
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
	pr_info("loaded; write \"<ops_per_thread> [threads]\" to /sys/kernel/debug/lhp_bench\n");
	return 0;
}
module_init(lhp_bench_init);

static void __exit lhp_bench_exit(void)
{
	debugfs_remove(lhp_bench_dentry);
}
module_exit(lhp_bench_exit);

MODULE_DESCRIPTION("Microbenchmark for the LHP 2M chunk allocator");
MODULE_LICENSE("GPL");
