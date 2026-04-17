/**
 * Sensor table + id allocation.
 *
 * Holds the set of currently-instantiated sensors regardless of protocol.
 * Each slot maps an external (wire-visible) id to a protocol_id plus an
 * internal_id that the protocol backend uses to index its own state pool.
 * Backends never touch the table directly -- they go through these helpers
 * so id allocation stays centralized.
 */

#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <stdint.h>

/* Across-protocols cap. Bounded well below 255 because every live sensor
 * owns a register map and a hardware peripheral; RAM + peripheral count
 * exhaust long before the id space does. Raise if a future need appears. */
#define SENSOR_MAX_SLOTS            8U

typedef struct
{
    uint8_t  id;             /* 0 = free slot; nonzero = PROTO_SENSOR_ID_MIN..MAX */
    uint8_t  protocol_id;    /* PROTO_ID_* */
    uint8_t  internal_id;    /* backend-specific index into its own state pool */
} sensor_slot_t;

/* Zero the table. Call once during startup. */
void sensor_manager_init(void);

/* Allocate a slot + fresh id for `protocol_id`, store `internal_id`.
 * Returns the new id (>=1) on success, PROTO_SENSOR_ID_NONE if no slot free. */
uint8_t sensor_manager_register(uint8_t protocol_id, uint8_t internal_id);

/* Find the slot for a given id. Returns NULL if `id` is not active. */
sensor_slot_t *sensor_manager_lookup(uint8_t id);

/* Mark a slot free. Does nothing if `id` is not active. */
void sensor_manager_release(uint8_t id);

#endif /* SENSOR_MANAGER_H */
