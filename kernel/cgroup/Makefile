# SPDX-License-Identifier: GPL-2.0

KCOV_INSTRUMENT_atomic.o := n

obj-y := cgroup.o rstat.o namespace.o cgroup-v1.o freezer.o

obj-$(CONFIG_MEMCG_ATOMIC_COUNTER) += atomic.o
obj-$(CONFIG_CGROUP_FREEZER) += legacy_freezer.o
obj-$(CONFIG_CGROUP_PIDS) += pids.o
obj-$(CONFIG_CGROUP_RDMA) += rdma.o
obj-$(CONFIG_CPUSETS) += cpuset.o
obj-$(CONFIG_CPUSETS_V1) += cpuset-v1.o
obj-$(CONFIG_CGROUP_MISC) += misc.o
obj-$(CONFIG_CGROUP_DMEM) += dmem.o
obj-$(CONFIG_CGROUP_DEBUG) += debug.o
