# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wall -pthread

all: ${HELPER_PROGS}

TEST_FILES     := with_stress.sh
TEST_PROGS     := test_stress.sh test_cpuset_prs.sh test_cpuset_v1_hp.sh
TEST_GEN_FILES := wait_inotify
# Keep the lists lexicographically sorted
TEST_GEN_PROGS  = test_core
TEST_GEN_PROGS += test_cpu
TEST_GEN_PROGS += test_cpuset
TEST_GEN_PROGS += test_freezer
TEST_GEN_PROGS += test_hugetlb_memcg
TEST_GEN_PROGS += test_kill
TEST_GEN_PROGS += test_kmem
TEST_GEN_PROGS += test_memcontrol
TEST_GEN_PROGS += test_pids
TEST_GEN_PROGS += test_zswap

# test_memcg_perf is handled separately with custom rules
TEST_GEN_PROGS += test_memcg_perf

LOCAL_HDRS += $(selfdir)/clone3/clone3_selftests.h $(selfdir)/pidfd/pidfd.h

include ../lib.mk
include lib/libcgroup.mk

# Simple rule for test_memcg_perf
$(OUTPUT)/test_memcg_perf: test_memcg_perf.c $(LIBCGROUP_O)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) $< $(LIBCGROUP_O) $(LDLIBS) -o $@

$(OUTPUT)/test_core: $(LIBCGROUP_O)
$(OUTPUT)/test_cpu: $(LIBCGROUP_O)
$(OUTPUT)/test_cpuset: $(LIBCGROUP_O)
$(OUTPUT)/test_freezer: $(LIBCGROUP_O)
$(OUTPUT)/test_hugetlb_memcg: $(LIBCGROUP_O)
$(OUTPUT)/test_kill: $(LIBCGROUP_O)
$(OUTPUT)/test_kmem: $(LIBCGROUP_O)
$(OUTPUT)/test_memcontrol: $(LIBCGROUP_O)
$(OUTPUT)/test_pids: $(LIBCGROUP_O)
$(OUTPUT)/test_zswap: $(LIBCGROUP_O)
