package checks

import (
	"time"

	model "github.com/DataDog/agent-payload/v5/process"
	"github.com/DataDog/datadog-agent/pkg/process/config"
	"github.com/DataDog/datadog-agent/pkg/process/util"
	"github.com/DataDog/datadog-agent/pkg/util/containers"
	containercollectors "github.com/DataDog/datadog-agent/pkg/util/containers/collectors"
	"github.com/DataDog/datadog-agent/pkg/util/log"
	"github.com/DataDog/datadog-agent/pkg/util/system"
)

// RTContainer is a singleton RTContainerCheck.
var RTContainer = &RTContainerCheck{}

// RTContainerCheck collects numeric statistics about live ctrList.
type RTContainerCheck struct {
	sysInfo   *model.SystemInfo
	lastRates map[string]util.ContainerRateMetrics
	lastRun   time.Time
}

// Init initializes a RTContainerCheck instance.
func (r *RTContainerCheck) Init(_ *config.AgentConfig, sysInfo *model.SystemInfo) {
	r.sysInfo = sysInfo
}

// Name returns the name of the RTContainerCheck.
func (r *RTContainerCheck) Name() string { return config.RTContainerCheckName }

// RealTime indicates if this check only runs in real-time mode.
func (r *RTContainerCheck) RealTime() bool { return true }

// Run runs the real-time container check getting container-level stats from the Cgroups and Docker APIs.
func (r *RTContainerCheck) Run(cfg *config.AgentConfig, groupID int32) ([]model.MessageBody, error) {
	ctrList, err := util.GetContainers()

	if err == containercollectors.ErrPermaFail || err == containercollectors.ErrNothingYet {
		log.Trace("container collector was not detected, container check will not return any data")
		return nil, nil
	}

	if err != nil {
		return nil, err
	}

	if len(ctrList) == 0 {
		log.Trace("no containers found")
		return nil, nil
	}

	// End check early if this is our first run.
	if r.lastRates == nil {
		r.lastRates = util.ExtractContainerRateMetric(ctrList)
		r.lastRun = time.Now()
		return nil, nil
	}

	groupSize := len(ctrList) / cfg.MaxPerMessage
	if len(ctrList)%cfg.MaxPerMessage != 0 {
		groupSize++
	}
	chunked := fmtContainerStats(ctrList, r.lastRates, r.lastRun, groupSize)
	messages := make([]model.MessageBody, 0, groupSize)
	for i := 0; i < groupSize; i++ {
		messages = append(messages, &model.CollectorContainerRealTime{
			HostName:          cfg.HostName,
			Stats:             chunked[i],
			NumCpus:           int32(system.HostCPUCount()),
			TotalMemory:       r.sysInfo.TotalMemory,
			GroupId:           groupID,
			GroupSize:         int32(groupSize),
			ContainerHostType: cfg.ContainerHostType,
		})
	}

	r.lastRates = util.ExtractContainerRateMetric(ctrList)
	r.lastRun = time.Now()

	return messages, nil
}

// fmtContainerStats formats and chunks the ctrList into a slice of chunks using a specific
// number of chunks. len(result) MUST EQUAL chunks.
func fmtContainerStats(
	ctrList []*containers.Container,
	lastRates map[string]util.ContainerRateMetrics,
	lastRun time.Time,
	chunks int,
) [][]*model.ContainerStat {
	perChunk := (len(ctrList) / chunks) + 1
	chunked := make([][]*model.ContainerStat, chunks)
	chunk := make([]*model.ContainerStat, 0, perChunk)
	i := 0
	for _, ctr := range ctrList {
		lastCtr, ok := lastRates[ctr.ID]
		if !ok {
			// Set to an empty container so rate calculations work and use defaults.
			lastCtr = util.NullContainerRates
		}

		// Just in case the container is found, but refs are nil.
		// Note some CPU values are set to -1, to be skipped on the backend, because they are reported cumulatively
		ctr = fillNilContainer(ctr)
		lastCtr = fillNilRates(lastCtr)

		ifStats := ctr.Network.SumInterfaces()
		cpus := system.HostCPUCount()
		sys2, sys1 := ctr.CPU.SystemUsage, lastCtr.CPU.SystemUsage

		userPct := calculateCtrPct(ctr.CPU.User, lastCtr.CPU.User, sys2, sys1, cpus, lastRun)
		systemPct := calculateCtrPct(ctr.CPU.System, lastCtr.CPU.System, sys2, sys1, cpus, lastRun)
		var totalPct float32
		if userPct == -1 || systemPct == -1 {
			totalPct = -1
		} else {
			totalPct = calculateCtrPct(ctr.CPU.User+ctr.CPU.System, lastCtr.CPU.User+lastCtr.CPU.System, sys2, sys1, cpus, lastRun)
		}

		chunk = append(chunk, &model.ContainerStat{
			Id:          ctr.ID,
			UserPct:     userPct,
			SystemPct:   systemPct,
			TotalPct:    totalPct,
			CpuLimit:    float32(ctr.Limits.CPULimit),
			MemRss:      ctr.Memory.RSS,
			MemCache:    ctr.Memory.Cache,
			MemLimit:    ctr.Limits.MemLimit,
			Rbps:        calculateRate(ctr.IO.ReadBytes, lastCtr.IO.ReadBytes, lastRun),
			Wbps:        calculateRate(ctr.IO.WriteBytes, lastCtr.IO.WriteBytes, lastRun),
			NetRcvdPs:   calculateRate(ifStats.PacketsRcvd, lastCtr.NetworkSum.PacketsRcvd, lastRun),
			NetSentPs:   calculateRate(ifStats.PacketsSent, lastCtr.NetworkSum.PacketsSent, lastRun),
			NetRcvdBps:  calculateRate(ifStats.BytesRcvd, lastCtr.NetworkSum.BytesRcvd, lastRun),
			NetSentBps:  calculateRate(ifStats.BytesSent, lastCtr.NetworkSum.BytesSent, lastRun),
			ThreadCount: ctr.CPU.ThreadCount,
			ThreadLimit: ctr.Limits.ThreadLimit,
			State:       model.ContainerState(model.ContainerState_value[ctr.State]),
			Health:      model.ContainerHealth(model.ContainerHealth_value[ctr.Health]),
			Started:     ctr.StartedAt,
		})
		if len(chunk) == perChunk {
			chunked[i] = chunk
			chunk = make([]*model.ContainerStat, 0, perChunk)
			i++
		}
	}
	// Add the last chunk if data remains.
	if len(chunk) > 0 {
		chunked[i] = chunk
	}
	return chunked
}
