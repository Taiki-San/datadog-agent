package metadata

import (
	"context"
	"expvar"
	"fmt"

	"github.com/DataDog/datadog-agent/pkg/config"
	"github.com/DataDog/datadog-agent/pkg/metadata/inventories"
	"github.com/DataDog/datadog-agent/pkg/util/log"

	"github.com/DataDog/datadog-agent/pkg/serializer"
	"github.com/DataDog/datadog-agent/pkg/util"
)

type inventoriesCollector struct {
	ac   inventories.AutoConfigInterface
	coll inventories.CollectorInterface
	sc   *Scheduler
}

func createPayload(ctx context.Context, ac inventories.AutoConfigInterface, coll inventories.CollectorInterface) (*inventories.Payload, error) {
	hostname, err := util.GetHostname(ctx)
	if err != nil {
		return nil, fmt.Errorf("unable to submit inventories metadata payload, no hostname: %s", err)
	}

	return inventories.GetPayload(ctx, hostname, ac, coll), nil
}

// Send collects the data needed and submits the payload
func (c inventoriesCollector) Send(ctx context.Context, s *serializer.Serializer) error {
	if s == nil {
		return nil
	}

	payload, err := createPayload(ctx, c.ac, c.coll)
	if err != nil {
		return err
	}

	if err := s.SendMetadata(payload); err != nil {
		return fmt.Errorf("unable to submit inventories payload, %s", err)
	}
	return nil
}

// Init initializes the inventory metadata collection. This should be called in
// all agents that wish to track inventory, after configuration is initialized.
func (c inventoriesCollector) Init() error {
	inventories.InitializeData()
	return inventories.StartMetadataUpdatedGoroutine(c.sc, config.GetInventoriesMinInterval())
}

// SetupInventoriesExpvar init the expvar function for inventories
func SetupInventoriesExpvar(ac inventories.AutoConfigInterface, coll inventories.CollectorInterface) {
	expvar.Publish("inventories", expvar.Func(func() interface{} {
		log.Debugf("Creating inventory payload for expvar")
		p, err := createPayload(context.TODO(), ac, coll)
		if err != nil {
			log.Errorf("Could not create inventory payload for expvar: %s", err)
			return &inventories.Payload{}
		}
		return p
	}))
}

// SetupInventories registers the inventories collector into the Scheduler and, if configured, schedules it
func SetupInventories(sc *Scheduler, ac inventories.AutoConfigInterface, coll inventories.CollectorInterface) error {
	ic := inventoriesCollector{
		ac:   ac,
		coll: coll,
		sc:   sc,
	}
	RegisterCollector("inventories", ic)

	if err := sc.AddCollector("inventories", config.GetInventoriesMaxInterval()); err != nil {
		return err
	}

	SetupInventoriesExpvar(ac, coll)

	return nil
}
