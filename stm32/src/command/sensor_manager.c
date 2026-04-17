#include "sensor_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <assert.h>

#include "protocol.h"

#define SENSOR_MANAGER_MAX_ALLOC_TRIES  PROTO_SENSOR_ID_MAX

/* Static table. Access is single-threaded (only the protocol task drives
 * setup/stop), so no mutex needed. If we ever let another task register
 * a sensor, wrap these in a critical section or mutex. */
static sensor_slot_t slots[SENSOR_MAX_SLOTS];

/* Last id handed out. Ids are assigned round-robin so freed slots don't
 * immediately get recycled -- keeps stale references on the BBB side from
 * silently pointing at a new sensor. */
static uint8_t next_id_hint = PROTO_SENSOR_ID_MIN;

void sensor_manager_init(void)
{
    for (unsigned i = 0; i < SENSOR_MAX_SLOTS; ++i)
    {
        slots[i].id            = PROTO_SENSOR_ID_NONE;
        slots[i].protocol_id   = 0U;
        slots[i].internal_id   = 0U;
    }
    next_id_hint = PROTO_SENSOR_ID_MIN;
}

/* Is `id` currently active in the table? */
static bool id_in_use(uint8_t id)
{
    for (unsigned i = 0; i < SENSOR_MAX_SLOTS; ++i)
    {
        if (slots[i].id == id)
        {
            return true;
        }
    }
    return false;
}

/* Pick the next id that isn't already allocated. If the whole 1..255 space
 * is full (can't happen under SENSOR_MAX_SLOTS=8) fall back to NONE. */
static uint8_t allocate_id(void)
{
    for (unsigned tries = 0; tries < SENSOR_MANAGER_MAX_ALLOC_TRIES; ++tries)
    {
        uint8_t candidate = next_id_hint;

        /* Advance hint, wrapping 0xFF -> 0x01 to keep NONE (0x00) reserved. */
        next_id_hint = (candidate == PROTO_SENSOR_ID_MAX)
                       ? PROTO_SENSOR_ID_MIN
                       : (uint8_t)(candidate + 1U);

        if (!id_in_use(candidate))
        {
            return candidate;
        }
    }
    return PROTO_SENSOR_ID_NONE;
}

uint8_t sensor_manager_register(uint8_t protocol_id, uint8_t internal_id)
{
    assert(protocol_id != PROTO_SENSOR_ID_NONE);

    sensor_slot_t *slot = NULL;
    for (unsigned i = 0; i < SENSOR_MAX_SLOTS; ++i)
    {
        if (slots[i].id == PROTO_SENSOR_ID_NONE)
        {
            slot = &slots[i];
            break;
        }
    }
    if (slot == NULL)
    {
        return PROTO_SENSOR_ID_NONE;
    }

    uint8_t id = allocate_id();
    if (id == PROTO_SENSOR_ID_NONE)
    {
        return PROTO_SENSOR_ID_NONE;
    }

    slot->id            = id;
    slot->protocol_id   = protocol_id;
    slot->internal_id   = internal_id;

    return id;
}

sensor_slot_t *sensor_manager_lookup(uint8_t id)
{
    if (id == PROTO_SENSOR_ID_NONE)
    {
        return NULL;
    }

    for (unsigned i = 0; i < SENSOR_MAX_SLOTS; ++i)
    {
        if (slots[i].id == id)
        {
            return &slots[i];
        }
    }
    return NULL;
}

void sensor_manager_release(uint8_t id)
{
    sensor_slot_t *slot = sensor_manager_lookup(id);
    if (slot == NULL)
    {
        return;
    }

    slot->id            = PROTO_SENSOR_ID_NONE;
    slot->protocol_id   = 0U;
    slot->internal_id   = 0U;
}
