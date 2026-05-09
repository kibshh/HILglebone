#include "i2c_sensor.h"

#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"

#include "helpers.h"
#include "protocol.h"
#include "sensor_manager.h"

/* ── Per-sensor state ─────────────────────────────────────────────── */

typedef struct
{
    bool      in_use;

    /* Copy of the validated wire config. Keeping the raw fields makes it
     * trivial to hand off to the hardware driver when it lands. */
    uint32_t  clock_hz;
    uint8_t   address_mode;
    uint16_t  primary_addr;
    uint16_t  secondary_addr;
    uint8_t   flags;
    uint8_t   reg_addr_width;
    uint8_t   reg_addr_endian;
    uint8_t   auto_inc_mode;
    uint16_t  register_count;
    uint16_t  response_delay_us;
    uint16_t  clock_stretch_max_us;

    /* Heap-allocated register map, length = register_count bytes. */
    uint8_t  *regmap;
} i2c_sensor_state_t;

/* Static pool, sized to the hardware cap. No heap allocation for the
 * bookkeeping itself -- only the variable-length register map goes on the
 * FreeRTOS heap. */
static i2c_sensor_state_t i2c_sensor_states[I2C_SENSOR_MAX_COUNT];

/* ── Slot helpers ─────────────────────────────────────────────────── */

#define I2C_INTERNAL_ID_NONE        0xFFU

static uint8_t alloc_slot(void)
{
    for (uint8_t i = 0; i < I2C_SENSOR_MAX_COUNT; ++i)
    {
        if (!i2c_sensor_states[i].in_use)
        {
            return i;
        }
    }
    return I2C_INTERNAL_ID_NONE;
}

static void free_slot(uint8_t idx)
{
    assert(idx < I2C_SENSOR_MAX_COUNT);

    i2c_sensor_state_t *s = &i2c_sensor_states[idx];

    if (s->regmap != NULL)
    {
        vPortFree(s->regmap);
    }

    memset(s, 0, sizeof(*s));
}

/* ── Validation ───────────────────────────────────────────────────── */

/* Validate the I2C cfg block. On success returns ERR_SUCCESS and fills
 * `out`; on failure returns an error code and `out` is left untouched.
 * `cfg` is the slice after the generic protocol_id byte. */
static uint8_t validate_cfg(const uint8_t      *cfg,
                            uint16_t            cfg_len,
                            i2c_sensor_state_t *out)
{
    assert(out != NULL);
    assert(cfg != NULL || cfg_len == 0U);

    if (cfg_len < I2C_CFG_SIZE_NO_PRESET)
    {
        return ERR_MALFORMED_PAYLOAD;
    }

    uint32_t clock_hz             = read_u32_le(&cfg[I2C_CFG_OFFSET_CLOCK_HZ]);
    uint8_t  address_mode         = cfg[I2C_CFG_OFFSET_ADDRESS_MODE];
    uint16_t primary_addr         = read_u16_le(&cfg[I2C_CFG_OFFSET_PRIMARY_ADDR]);
    uint16_t secondary_addr       = read_u16_le(&cfg[I2C_CFG_OFFSET_SECONDARY_ADDR]);
    uint8_t  flags                = cfg[I2C_CFG_OFFSET_FLAGS];
    uint8_t  reg_addr_width       = cfg[I2C_CFG_OFFSET_REG_ADDR_WIDTH];
    uint8_t  reg_addr_endian      = cfg[I2C_CFG_OFFSET_REG_ADDR_ENDIAN];
    uint8_t  auto_inc_mode        = cfg[I2C_CFG_OFFSET_AUTO_INC_MODE];
    uint16_t register_count       = read_u16_le(&cfg[I2C_CFG_OFFSET_REGISTER_COUNT]);
    uint16_t response_delay_us    = read_u16_le(&cfg[I2C_CFG_OFFSET_RESPONSE_DELAY_US]);
    uint16_t clock_stretch_max_us = read_u16_le(&cfg[I2C_CFG_OFFSET_CLOCK_STRETCH_MAX_US]);

    /* reserved flag bit must be 0 */
    if (flags & I2C_FLAG_RESERVED_MASK)
    {
        return ERR_INVALID_PARAMETER;
    }

    /* Address mode */
    if (address_mode != I2C_ADDRESS_MODE_7BIT &&
        address_mode != I2C_ADDRESS_MODE_10BIT)
    {
        return ERR_I2C_BAD_ADDR_MODE;
    }

    /* Secondary only allowed in 7-bit mode */
    if (address_mode == I2C_ADDRESS_MODE_10BIT && secondary_addr != 0U)
    {
        return ERR_I2C_BAD_ADDR_MODE;
    }

    /* Address bounds / reserved ranges */
    if (address_mode == I2C_ADDRESS_MODE_7BIT)
    {
        if (primary_addr <= I2C_7BIT_RESERVED_LOW ||
            primary_addr >= I2C_7BIT_RESERVED_HIGH)
        {
            return ERR_I2C_ADDR_RESERVED;
        }

        if (secondary_addr != 0U &&
            (secondary_addr <= I2C_7BIT_RESERVED_LOW ||
             secondary_addr >= I2C_7BIT_RESERVED_HIGH))
        {
            return ERR_I2C_ADDR_RESERVED;
        }
    }
    else    /* 10-bit */
    {
        if (primary_addr == 0U || primary_addr > I2C_10BIT_ADDR_MAX)
        {
            return ERR_I2C_ADDR_RESERVED;
        }
    }

    /* Clock */
    if (clock_hz < I2C_SENSOR_CLOCK_MIN_HZ || clock_hz > I2C_SENSOR_CLOCK_MAX_HZ)
    {
        return ERR_I2C_CLOCK_UNSUPPORTED;
    }

    /* Register-map size */
    if (register_count == 0U || register_count > I2C_SENSOR_REGMAP_MAX)
    {
        return ERR_I2C_REGMAP_TOO_LARGE;
    }

    /* Enum fields */
    if (auto_inc_mode > I2C_AUTO_INC_MODE_BOTH ||
        reg_addr_width > I2C_REG_ADDR_WIDTH_16 ||
        reg_addr_endian > I2C_REG_ADDR_ENDIAN_LITTLE)
    {
        return ERR_INVALID_PARAMETER;
    }

    /* PEC requires SMBus mode */
    if ((flags & I2C_FLAG_PEC_REQUIRED) && !(flags & I2C_FLAG_SMBUS_MODE))
    {
        return ERR_I2C_SMBUS_REQUIRED;
    }

    /* All checks passed -- commit to `out`. */
    out->clock_hz              = clock_hz;
    out->address_mode          = address_mode;
    out->primary_addr          = primary_addr;
    out->secondary_addr        = secondary_addr;
    out->flags                 = flags;
    out->reg_addr_width        = reg_addr_width;
    out->reg_addr_endian       = reg_addr_endian;
    out->auto_inc_mode         = auto_inc_mode;
    out->register_count        = register_count;
    out->response_delay_us     = response_delay_us;
    out->clock_stretch_max_us  = clock_stretch_max_us;

    return ERR_SUCCESS;
}

/* ── Public API ───────────────────────────────────────────────────── */

void i2c_sensor_init(void)
{
    for (unsigned i = 0; i < I2C_SENSOR_MAX_COUNT; ++i)
    {
        i2c_sensor_states[i].in_use = false;
        i2c_sensor_states[i].regmap = NULL;
    }
}

uint8_t i2c_sensor_setup(const uint8_t *cfg, uint16_t cfg_len, uint8_t *out_sensor_id)
{
    assert(cfg != NULL || cfg_len == 0U);
    assert(out_sensor_id != NULL);

    *out_sensor_id = PROTO_SENSOR_ID_NONE;

    uint8_t idx = alloc_slot();
    if (idx == I2C_INTERNAL_ID_NONE)
    {
        return ERR_I2C_NO_FREE_PERIPHERAL;
    }

    i2c_sensor_state_t *s = &i2c_sensor_states[idx];

    uint8_t err = validate_cfg(cfg, cfg_len, s);
    if (err != ERR_SUCCESS)
    {
        return err;
    }

    uint8_t  has_preset       = cfg[I2C_CFG_OFFSET_HAS_PRESET];
    uint16_t preset_reg_start = 0U;
    uint16_t preset_value_len = 0U;

    if (has_preset == I2C_CFG_HAS_PRESET_YES)
    {
        if (cfg_len < I2C_CFG_SIZE_WITH_PRESET_HEADER)
        {
            return ERR_MALFORMED_PAYLOAD;
        }

        preset_reg_start = read_u16_le(&cfg[I2C_CFG_OFFSET_PRESET_REG_START]);
        preset_value_len = read_u16_le(&cfg[I2C_CFG_OFFSET_PRESET_VALUES_LEN]);

        if (preset_value_len == 0U)
        {
            return ERR_INVALID_PARAMETER;
        }
        if (cfg_len < (uint16_t)(I2C_CFG_SIZE_WITH_PRESET_HEADER + preset_value_len))
        {
            return ERR_MALFORMED_PAYLOAD;
        }
        if ((uint32_t)preset_reg_start + preset_value_len > s->register_count)
        {
            return ERR_I2C_REGISTER_OOB;
        }
    }
    else if (has_preset != I2C_CFG_HAS_PRESET_NO)
    {
        return ERR_INVALID_PARAMETER;
    }

    s->regmap = pvPortMalloc(s->register_count);
    if (s->regmap == NULL)
    {
        return ERR_OUT_OF_RESOURCES;
    }
    memset(s->regmap, 0, s->register_count);

    if (has_preset == I2C_CFG_HAS_PRESET_YES)
    {
        memcpy(&s->regmap[preset_reg_start],
               &cfg[I2C_CFG_OFFSET_PRESET_VALUES],
               preset_value_len);
    }

    s->in_use = true;

    uint8_t id = sensor_manager_register(PROTO_ID_I2C, idx);
    if (id == PROTO_SENSOR_ID_NONE)
    {
        free_slot(idx);
        return ERR_OUT_OF_RESOURCES;
    }

    *out_sensor_id = id;
    return ERR_SUCCESS;
}

uint8_t i2c_sensor_set_output(uint8_t internal_id, const uint8_t *values, uint16_t values_len)
{

    assert(values != NULL || values_len == 0U);
    assert(internal_id < I2C_SENSOR_MAX_COUNT);

    i2c_sensor_state_t *s = &i2c_sensor_states[internal_id];

    if (!s->in_use)
    {
        return ERR_INVALID_SENSOR_ID;
    }

    if (values_len < I2C_SET_OUTPUT_HEADER_SIZE)
    {
        return ERR_MALFORMED_PAYLOAD;
    }

    uint16_t reg_start  = read_u16_le(&values[I2C_SET_OUTPUT_OFFSET_REG_START]);
    uint16_t write_len  = read_u16_le(&values[I2C_SET_OUTPUT_OFFSET_VALUE_LEN]);

    if (write_len == 0U)
    {
        return ERR_INVALID_PARAMETER;
    }
    if (values_len < (uint16_t)(I2C_SET_OUTPUT_HEADER_SIZE + write_len))
    {
        return ERR_MALFORMED_PAYLOAD;
    }

    if (reg_start > s->register_count ||
        write_len > (s->register_count - reg_start))
    {
        return ERR_I2C_REGISTER_OOB;
    }

    assert(s->regmap != NULL);

    memcpy(&s->regmap[reg_start],
           &values[I2C_SET_OUTPUT_OFFSET_VALUES],
           write_len);

    return ERR_SUCCESS;
}

uint8_t i2c_sensor_stop(uint8_t internal_id)
{
    assert(internal_id < I2C_SENSOR_MAX_COUNT);

    free_slot(internal_id);
    return ERR_SUCCESS;
}
