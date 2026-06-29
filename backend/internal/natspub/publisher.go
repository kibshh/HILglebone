// Package natspub publishes commands to NATS, routing each envelope to the
// target BBB's device-scoped subject. session_id is carried in the envelope
// body for the BBB to sanity-check against its current session, but it is
// not used for routing — routing is by device_id so each BBB has its own
// subject and the workqueue stream's "delivered to exactly one consumer"
// semantics are unambiguous.
package natspub

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/nats-io/nats.go"
	"google.golang.org/protobuf/proto"

	pb "github.com/kibshh/HILglebone/backend/gen/hilglebone/v1"
)

const (
	commandSubjectFmt = "device.%s.command"
	otaSubjectFmt     = "device.%s.ota"
	connectTimeout    = 5 * time.Second
	publishTimeout    = 3 * time.Second
)

// Publisher owns a NATS+JetStream connection and routes proto envelopes by
// subject. Connections are long-lived; create once at startup and Close at
// shutdown.
//
// All publishes go through JetStream so the broker acknowledges persistence
// to the stream before the call returns. A core nats.Publish would buffer
// locally and return immediately, making "publish failed" detection
// impossible — JetStream's ack-waiting publish gives us the strict
// publish-or-fail semantic the session service depends on for rollback.
type Publisher struct {
	nc *nats.Conn
	js nats.JetStreamContext
}

func Open(url string) (*Publisher, error) {
	if url == "" {
		return nil, errors.New("nats url is empty")
	}
	nc, err := nats.Connect(url,
		nats.Timeout(connectTimeout),
		nats.MaxReconnects(-1),
		nats.ReconnectWait(2*time.Second),
	)
	if err != nil {
		return nil, fmt.Errorf("nats connect: %w", err)
	}
	js, err := nc.JetStream()
	if err != nil {
		nc.Close()
		return nil, fmt.Errorf("jetstream context: %w", err)
	}
	return &Publisher{nc: nc, js: js}, nil
}

func (p *Publisher) Close() {
	if p == nil || p.nc == nil {
		return
	}
	_ = p.nc.Drain()
}

// PublishCommand marshals env and publishes it to device.{device_id}.command
// via JetStream. The publish blocks until the broker acks (or the publish
// timeout fires); a returned error means the message did NOT reach the stream
// and the caller should consider the operation failed.
//
// The routing subject is derived from env.DeviceId so the on-wire body and
// the channel can never disagree. MessageId is forwarded as the Nats-Msg-Id
// header so JetStream can deduplicate retried publishes server-side within
// the stream's dupe-window.
func (p *Publisher) PublishCommand(ctx context.Context, env *pb.CommandEnvelope) error {
	if err := validateCommandEnvelope(env); err != nil {
		return err
	}
	payload, err := proto.Marshal(env)
	if err != nil {
		return fmt.Errorf("marshal command envelope: %w", err)
	}
	subject := fmt.Sprintf(commandSubjectFmt, env.DeviceId)
	return p.publish(ctx, subject, payload, env.MessageId)
}

// PublishOTA marshals env and publishes it to device.{device_id}.ota.
func (p *Publisher) PublishOTA(ctx context.Context, env *pb.OtaEnvelope) error {
	if err := validateOtaEnvelope(env); err != nil {
		return err
	}
	payload, err := proto.Marshal(env)
	if err != nil {
		return fmt.Errorf("marshal ota envelope: %w", err)
	}
	subject := fmt.Sprintf(otaSubjectFmt, env.DeviceId)
	return p.publish(ctx, subject, payload, env.MessageId)
}

func validateCommandEnvelope(env *pb.CommandEnvelope) error {
	if env == nil {
		return errors.New("envelope is nil")
	}
	if env.SessionId == "" {
		return errors.New("envelope missing session_id")
	}
	if env.DeviceId == "" {
		return errors.New("envelope missing device_id")
	}
	if env.MessageId == "" {
		return errors.New("envelope missing message_id")
	}
	if env.Payload == nil {
		return errors.New("envelope payload variant is unset")
	}
	return nil
}

func validateOtaEnvelope(env *pb.OtaEnvelope) error {
	if env == nil {
		return errors.New("envelope is nil")
	}
	if env.SessionId == "" {
		return errors.New("envelope missing session_id")
	}
	if env.DeviceId == "" {
		return errors.New("envelope missing device_id")
	}
	if env.MessageId == "" {
		return errors.New("envelope missing message_id")
	}
	return nil
}

func (p *Publisher) publish(ctx context.Context, subject string, payload []byte, msgID string) error {
	msg := &nats.Msg{
		Subject: subject,
		Data:    payload,
		Header:  nats.Header{nats.MsgIdHdr: []string{msgID}},
	}
	pubCtx, cancel := context.WithTimeout(ctx, publishTimeout)
	defer cancel()
	if _, err := p.js.PublishMsg(msg, nats.Context(pubCtx)); err != nil {
		return fmt.Errorf("jetstream publish %s: %w", subject, err)
	}
	return nil
}
