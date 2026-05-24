/**
 * Firmware-internal error codes.
 *
 * Used by low-level drivers to communicate failure reasons without
 * depending on the wire protocol error code space.  At the protocol
 * boundary (sensor backends, dispatcher) these are mapped to the
 * appropriate ERR_* protocol codes before being returned to the BBB.
 *
 * Asserts catch programmer errors (NULL pointers, out-of-range enum
 * values that were validated by the caller).  err_code_t covers
 * runtime failures (resource exhaustion, hardware busy, etc.) that
 * callers must handle.
 */

#ifndef ERR_CODES_H
#define ERR_CODES_H

typedef enum
{
    ERR_CODE_OK       = 0,   /* operation completed successfully */
    ERR_CODE_ARG      = 1,   /* invalid argument (runtime-checkable value from external source) */
    ERR_CODE_BUSY     = 2,   /* resource is already in use */
    ERR_CODE_RESOURCES= 3,   /* no free slots / memory */
    ERR_CODE_CONFLICT = 4,   /* resource already configured differently */
    ERR_CODE_TIMEOUT  = 5,   /* operation did not complete in time */
    ERR_CODE_INTERNAL = 6,   /* unexpected internal state */
    ERR_CODE_EMPTY    = 7,   /* buffer/container had nothing to return (not a fault) */
} err_code_t;

#endif /* ERR_CODES_H */
