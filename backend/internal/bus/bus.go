// Package bus is an in-memory fan-out pub/sub for session-scoped events.
//
// Producers (NATS subscriber, sessions service) call Publish.
// Consumers (WebSocket handlers, future workers) call Subscribe(sessionID) and
// receive only events whose Event.SessionID matches.
//
// The bus is intentionally simple: bounded per-subscriber buffers, drops on
// full. Subscribers that can't keep up lose messages — acceptable for UI fan-out
// where the next update overwrites stale state anyway. Durable delivery and
// replay are JetStream's job, not the bus's.
package bus

import (
	"sync"
	"sync/atomic"
)

// Event is one session-scoped notification fanned out to subscribers.
// JSON is pre-encoded so each subscriber can write it to the wire without
// re-serializing — the bus is on the hot path; encoding once amortizes.
type Event struct {
	SessionID string
	DeviceID  string
	Type      string
	JSON      []byte
}

// Channel buffer per Subscription. Full buffer = drop on the floor (see
// Publish); the channel stays open.
const subscriberBuffer = 64

// When a subscription's dropped counter crosses this threshold, the bus
// calls sub.Close(). That closes sub.Done() — which the WS handler's
// watcher goroutine listens on and uses to force-close the underlying
// connection, unblocking any in-flight conn.Write. 256 is ~4x the buffer:
// enough to absorb bursts but small enough that a chronically slow client
// gets cut well before the 10s write timeout would otherwise fire.
const dropThreshold = 256

type Subscription struct {
	id        uint64
	sessionID string
	ch        chan Event
	done      chan struct{} // closed by Close so consumers can exit without
	//                         draining buffered events first.
	bus       *Bus
	dropped   atomic.Uint64
	closeOnce sync.Once
}

type Bus struct {
	mu     sync.RWMutex
	subs   map[uint64]*Subscription
	nextID atomic.Uint64
}

func New() *Bus {
	return &Bus{subs: make(map[uint64]*Subscription)}
}

// Publish delivers ev to every matching subscriber. Subscribers whose channel
// is full lose this event; the dropped counter on the subscription is bumped
// so a future health endpoint can surface "we have slow consumers".
//
// When a subscriber's drop count first hits dropThreshold, it's queued for
// proactive close (out-of-band so we don't try to take the write lock while
// holding the read lock here).
func (b *Bus) Publish(ev Event) {
	var escalate []*Subscription
	b.mu.RLock()
	for _, sub := range b.subs {
		if sub.sessionID != "" && sub.sessionID != ev.SessionID {
			continue
		}
		// Non-blocking send: drop on full buffer so one slow subscriber
		// can't stall the bus.
		select {
		case sub.ch <- ev:
		default:
			// atomic.Add returns the new value; using == (not >=) ensures
			// exactly one goroutine queues this sub for close even under
			// concurrent drops.
			if sub.dropped.Add(1) == dropThreshold {
				escalate = append(escalate, sub)
			}
		}
	}
	b.mu.RUnlock()
	for _, sub := range escalate {
		sub.Close()
	}
}

// Subscribe registers a subscription filtered by sessionID. Pass "" to receive
// every event (admin/debug; not used by the WS handler).
//
// The caller MUST eventually call Close on the returned subscription or the
// channel and slot remain allocated forever.
func (b *Bus) Subscribe(sessionID string) *Subscription {
	id := b.nextID.Add(1)
	sub := &Subscription{
		id:        id,
		sessionID: sessionID,
		ch:        make(chan Event, subscriberBuffer),
		done:      make(chan struct{}),
		bus:       b,
	}
	b.mu.Lock()
	b.subs[id] = sub
	b.mu.Unlock()
	return sub
}

// Events returns the receive channel. Closed by Close.
func (s *Subscription) Events() <-chan Event { return s.ch }

// Done is closed as soon as Close is called. Consumers should select on it
// in their main loop so they can exit immediately on escalation/teardown
// without first draining any events still buffered in the Events channel.
func (s *Subscription) Done() <-chan struct{} { return s.done }

// Dropped returns the number of events the bus discarded because this
// subscription's buffer was full.
func (s *Subscription) Dropped() uint64 { return s.dropped.Load() }

// Close removes the subscription from the bus and closes the event channel.
// Idempotent.
func (s *Subscription) Close() {
	s.closeOnce.Do(func() {
		s.bus.mu.Lock()
		delete(s.bus.subs, s.id)
		s.bus.mu.Unlock()
		// Close done before the events channel: a consumer in select can
		// pick the Done case and return without processing any final
		// buffered events first. Strictly cosmetic — both closes happen
		// in this same call, so a consumer that wakes up after both have
		// fired picks randomly between Done and the drained events
		// channel. Either way it exits.
		close(s.done)
		close(s.ch)
	})
}
