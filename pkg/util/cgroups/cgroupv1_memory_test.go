// Unless explicitly stated otherwise all files in this repository are licensed
// under the Apache License Version 2.0.
// This product includes software developed at Datadog (https://www.datadoghq.com/).
// Copyright 2016-present Datadog, Inc.

//go:build linux
// +build linux

package cgroups

import (
	"testing"

	"github.com/google/go-cmp/cmp"
	"github.com/stretchr/testify/assert"
)

const (
	sampleUsageTotal = "354488320"
	sampleMemoryStat = `cache 4866048
rss 19058688
rss_huge 0
shmem 0
mapped_file 0
dirty 0
writeback 0
swap 0
pgpgin 878460
pgpgout 872515
pgfault 879450
pgmajfault 0
inactive_anon 0
active_anon 18923520
inactive_file 4595712
active_file 0
unevictable 0
hierarchical_memory_limit 107108864
hierarchical_memsw_limit 9223372036854771712
total_cache 4866048
total_rss 19058688
total_rss_huge 0
total_shmem 0
total_mapped_file 0
total_dirty 0
total_writeback 0
total_swap 0
total_pgpgin 878460
total_pgpgout 872515
total_pgfault 879450
total_pgmajfault 0
total_inactive_anon 0
total_active_anon 18923520
total_inactive_file 4595712
total_active_file 0
total_unevictable 0`
	sampleMemoryFailCnt   = "0"
	sampleMemoryKmemUsage = "4444160"
	sampleMemoryLimit     = "67108864"
	sampleMemorySoftLimit = "9223372036854771712" // No limit
	sampleMemorySwapLimit = "9223372036854771712" // No limit
)

func createCgroupV1FakeMemoryFiles(cfs *cgroupMemoryFS, cg *cgroupV1) {
	cfs.setCgroupV1File(cg, "memory", "memory.usage_in_bytes", sampleUsageTotal)
	cfs.setCgroupV1File(cg, "memory", "memory.stat", sampleMemoryStat)
	cfs.setCgroupV1File(cg, "memory", "memory.failcnt", sampleMemoryFailCnt)
	cfs.setCgroupV1File(cg, "memory", "memory.kmem.usage_in_bytes", sampleMemoryKmemUsage)
	cfs.setCgroupV1File(cg, "memory", "memory.limit_in_bytes", sampleMemoryLimit)
	cfs.setCgroupV1File(cg, "memory", "memory.soft_limit_in_bytes", sampleMemorySoftLimit)
	cfs.setCgroupV1File(cg, "memory", "memory.memsw.limit_in_bytes", sampleMemorySwapLimit)
}

func TestCgroupV1MemoryStats(t *testing.T) {
	cfs := newCgroupMemoryFS("/test/fs/cgroup")

	var err error
	stats := &MemoryStats{}

	// Test failure if controller missing (memory is missing)
	tr.reset()
	cgFoo1 := cfs.createCgroupV1("foo1", containerCgroupKubePod(false))
	err = cgFoo1.GetMemoryStats(stats)
	assert.ErrorIs(t, err, &ControllerNotFoundError{Controller: "memory"})

	// Test reading files in memory controller, all files missing (memsw not counted as considered optional)
	tr.reset()
	cfs.enableControllers("memory")
	err = cgFoo1.GetMemoryStats(stats)
	assert.NoError(t, err)
	assert.Equal(t, len(tr.errors), 6)
	assert.Equal(t, "", cmp.Diff(MemoryStats{}, *stats))

	// Test reading files in memory controller, all files present
	tr.reset()
	createCgroupV1FakeMemoryFiles(cfs, cgFoo1)
	err = cgFoo1.GetMemoryStats(stats)
	assert.NoError(t, err)
	assert.ElementsMatch(t, []error{}, tr.errors)
	assert.Equal(t, "", cmp.Diff(MemoryStats{
		UsageTotal:   uint64Ptr(354488320),
		Cache:        uint64Ptr(4866048),
		Swap:         uint64Ptr(0),
		RSS:          uint64Ptr(19058688),
		RSSHuge:      uint64Ptr(0),
		MappedFile:   uint64Ptr(0),
		Pgpgin:       uint64Ptr(878460),
		Pgpgout:      uint64Ptr(872515),
		Pgfault:      uint64Ptr(879450),
		Pgmajfault:   uint64Ptr(0),
		InactiveAnon: uint64Ptr(0),
		ActiveAnon:   uint64Ptr(18923520),
		InactiveFile: uint64Ptr(4595712),
		ActiveFile:   uint64Ptr(0),
		Unevictable:  uint64Ptr(0),
		OOMEvents:    uint64Ptr(0),
		Limit:        uint64Ptr(67108864),
		KernelMemory: uint64Ptr(4444160),
	}, *stats))
}
