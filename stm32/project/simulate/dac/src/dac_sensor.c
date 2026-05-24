#include "dac_sensor.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "err_codes.h"
#include "gpio.h"
#include "helpers.h"
#include "protocol.h"
#include "sensor_manager.h"
#include "spi_master.h"

typedef struct
{
    bool                  initialized;
    spi_master_periph_t   periph;
    uint32_t              clock_hz;
    spi_master_mode_t     spi_mode;
    gpio_pin_t            mosi_pin;
    gpio_pin_t            sck_pin;
    gpio_pin_t            cs_pin;
    gpio_pin_t            ldac_pin;
    bool                  ldac_enabled;
    uint8_t               channels_active;
    SemaphoreHandle_t     tx_done;
    uint8_t               tx_buf[DAC_SPI_FRAME_SIZE];
} dac_chip_t;

static dac_chip_t chips[SPI_MASTER_COUNT];

typedef struct
{
    bool                in_use;
    spi_master_periph_t periph;
    uint8_t             channel;
} dac_sensor_state_t;

static dac_sensor_state_t states[DAC_SENSOR_MAX_COUNT];

static void chip_teardown(dac_chip_t *chip)
{
    (void)spi_master_deinit(chip->periph);
    vSemaphoreDelete(chip->tx_done);
    gpio_reset_to_defaults(chip->mosi_pin);
    gpio_reset_to_defaults(chip->sck_pin);
    gpio_reset_to_defaults(chip->cs_pin);
    if (chip->ldac_enabled) gpio_reset_to_defaults(chip->ldac_pin);
    chip->initialized = false;
}

static uint8_t slot_for(spi_master_periph_t periph, uint8_t channel)
{
    return (uint8_t)((uint8_t)periph * DAC_CHANNELS_PER_CHIP + channel);
}

static void build_frame(uint8_t out[DAC_SPI_FRAME_SIZE], uint8_t command, uint8_t address, uint16_t data)
{
    uint32_t frame = ((uint32_t)(command & 0xFU) << 24)
                   | ((uint32_t)(address & 0xFU) << 20)
                   | ((uint32_t)data << 4);
    write_u32_be(out, frame);
}

static void spi_done_callback(void *ctx)
{
    dac_chip_t *chip = (dac_chip_t *)ctx;
    gpio_set(chip->cs_pin);
    if (chip->ldac_enabled)
    {
        gpio_reset(chip->ldac_pin);
        gpio_set(chip->ldac_pin);
    }
    BaseType_t higher_prio_woken = pdFALSE;
    xSemaphoreGiveFromISR(chip->tx_done, &higher_prio_woken);
    portYIELD_FROM_ISR(higher_prio_woken);
}

static err_code_t chip_write(dac_chip_t *chip)
{
    assert(chip != NULL);
    gpio_reset(chip->cs_pin);
    if (spi_master_write(chip->periph, chip->tx_buf, DAC_SPI_FRAME_SIZE, spi_done_callback, chip) != ERR_SUCCESS)
    {
        gpio_set(chip->cs_pin);
        return ERR_INTERNAL;
    }
    if (xSemaphoreTake(chip->tx_done, pdMS_TO_TICKS(DAC_SPI_TX_TIMEOUT_MS)) != pdTRUE)
    {
        return ERR_INTERNAL;
    }
    return ERR_SUCCESS;
}

void dac_sensor_init(void)
{
    memset(chips,  0, sizeof(chips));
    memset(states, 0, sizeof(states));
}

err_code_t dac_sensor_setup(const uint8_t *cfg, uint16_t cfg_len, uint8_t *out_sensor_id)
{
    assert(cfg != NULL || cfg_len == 0U);
    assert(out_sensor_id != NULL);
    *out_sensor_id = PROTO_SENSOR_ID_NONE;
    if (cfg_len < DAC_CFG_SIZE) return ERR_MALFORMED_PAYLOAD;

    uint8_t  spi_periph_u8 = cfg[DAC_CFG_OFFSET_SPI_PERIPH];
    uint32_t clock_hz      = read_u32_le(&cfg[DAC_CFG_OFFSET_SPI_CLOCK_HZ]);
    uint8_t  mosi_port     = cfg[DAC_CFG_OFFSET_MOSI_PORT];
    uint8_t  mosi_pin      = cfg[DAC_CFG_OFFSET_MOSI_PIN];
    uint8_t  mosi_af       = cfg[DAC_CFG_OFFSET_MOSI_AF];
    uint8_t  sck_port      = cfg[DAC_CFG_OFFSET_SCK_PORT];
    uint8_t  sck_pin_val   = cfg[DAC_CFG_OFFSET_SCK_PIN];
    uint8_t  sck_af        = cfg[DAC_CFG_OFFSET_SCK_AF];
    uint8_t  cs_port       = cfg[DAC_CFG_OFFSET_CS_PORT];
    uint8_t  cs_pin_val    = cfg[DAC_CFG_OFFSET_CS_PIN];
    uint8_t  ldac_port     = cfg[DAC_CFG_OFFSET_LDAC_PORT];
    uint8_t  ldac_pin_val  = cfg[DAC_CFG_OFFSET_LDAC_PIN];
    uint8_t  channel       = cfg[DAC_CFG_OFFSET_CHANNEL];
    uint8_t  reference     = cfg[DAC_CFG_OFFSET_REFERENCE];
    uint8_t  spi_mode_u8   = cfg[DAC_CFG_OFFSET_SPI_MODE];
    uint16_t initial_value = read_u16_le(&cfg[DAC_CFG_OFFSET_INITIAL_VALUE]);

    bool ldac_en = (ldac_port != DAC_LDAC_PORT_DISABLED);

    if (spi_periph_u8 >= (uint8_t)SPI_MASTER_COUNT ||
        clock_hz == 0U ||
        mosi_port > (uint8_t)GPIO_PORT_MAX || mosi_pin > GPIO_PIN_MAX ||
        mosi_af == 0U || mosi_af > GPIO_AF_MAX ||
        sck_port > (uint8_t)GPIO_PORT_MAX || sck_pin_val > GPIO_PIN_MAX ||
        sck_af == 0U || sck_af > GPIO_AF_MAX ||
        cs_port > (uint8_t)GPIO_PORT_MAX || cs_pin_val > GPIO_PIN_MAX ||
        (ldac_en && (ldac_port > (uint8_t)GPIO_PORT_MAX || ldac_pin_val > GPIO_PIN_MAX)) ||
        channel > DAC_CHANNEL_MAX || reference > DAC_REFERENCE_MAX ||
        spi_mode_u8 > (uint8_t)SPI_MASTER_MODE_MAX)
    {
        return ERR_INVALID_PARAMETER;
    }

    spi_master_periph_t periph   = (spi_master_periph_t)spi_periph_u8;
    spi_master_mode_t   spi_mode = (spi_master_mode_t)spi_mode_u8;

    uint32_t max_clock = (periph == SPI_MASTER_SPI1)
                         ? SPI_MASTER_SPI1_MAX_CLOCK_HZ
                         : SPI_MASTER_SPI2_MAX_CLOCK_HZ;
    if (clock_hz > max_clock)
    {
        return ERR_INVALID_PARAMETER;
    }

    dac_chip_t *chip = &chips[periph];

    if (chip->initialized)
    {
        if (chip->clock_hz != clock_hz || chip->spi_mode != spi_mode)
            return ERR_DAC_CLOCK_MISMATCH;
        if (chip->mosi_pin.port != gpio_port_handle((gpio_port_t)mosi_port) || chip->mosi_pin.pin != mosi_pin ||
            chip->sck_pin.port  != gpio_port_handle((gpio_port_t)sck_port)  || chip->sck_pin.pin  != sck_pin_val ||
            chip->cs_pin.port   != gpio_port_handle((gpio_port_t)cs_port)   || chip->cs_pin.pin   != cs_pin_val ||
            chip->ldac_pin.port != gpio_port_handle((gpio_port_t)ldac_port) || chip->ldac_pin.pin != ldac_pin_val)
            return ERR_DAC_PIN_MISMATCH;
        if (chip->channels_active & (1U << channel))
            return ERR_DAC_CHANNEL_IN_USE;
    }

    uint8_t idx = slot_for(periph, channel);
    if (states[idx].in_use) return ERR_DAC_CHANNEL_IN_USE;

    if (!chip->initialized)
    {
        /* All validation passed — build gpio descriptors, then configure hardware. */
        gpio_pin_t mosi_gpio = gpio_make_pin((gpio_port_t)mosi_port, mosi_pin);
        gpio_pin_t sck_gpio  = gpio_make_pin((gpio_port_t)sck_port,  sck_pin_val);
        gpio_pin_t cs_gpio   = gpio_make_pin((gpio_port_t)cs_port,   cs_pin_val);
        gpio_pin_t ldac_gpio = {0};

        gpio_enable_clock((gpio_port_t)mosi_port);
        gpio_af_config_t mosi_cfg = { .pin = mosi_gpio, .af = mosi_af, .speed = GPIO_SPEED_HIGH, .pull = GPIO_PULL_NONE };
        gpio_configure_af(&mosi_cfg);

        gpio_enable_clock((gpio_port_t)sck_port);
        gpio_af_config_t sck_cfg = { .pin = sck_gpio, .af = sck_af, .speed = GPIO_SPEED_HIGH, .pull = GPIO_PULL_NONE };
        gpio_configure_af(&sck_cfg);

        gpio_enable_clock((gpio_port_t)cs_port);
        gpio_output_config_t cs_cfg = { .pin = cs_gpio, .output_type = GPIO_OUTPUT_PUSH_PULL, .speed = GPIO_SPEED_HIGH, .pull = GPIO_PULL_NONE };
        gpio_configure_output(&cs_cfg, DIGITAL_OUT_LEVEL_HIGH);

        if (ldac_en)
        {
            ldac_gpio = gpio_make_pin((gpio_port_t)ldac_port, ldac_pin_val);
            gpio_enable_clock((gpio_port_t)ldac_port);
            gpio_output_config_t ldac_cfg = { .pin = ldac_gpio, .output_type = GPIO_OUTPUT_PUSH_PULL, .speed = GPIO_SPEED_HIGH, .pull = GPIO_PULL_NONE };
            gpio_configure_output(&ldac_cfg, DIGITAL_OUT_LEVEL_HIGH);
        }

        SemaphoreHandle_t sem = xSemaphoreCreateBinary();
        if (sem == NULL)
        {
            gpio_reset_to_defaults(mosi_gpio);
            gpio_reset_to_defaults(sck_gpio);
            gpio_reset_to_defaults(cs_gpio);
            if (ldac_en) gpio_reset_to_defaults(ldac_gpio);
            return ERR_OUT_OF_RESOURCES;
        }

        if (spi_master_init(periph, clock_hz, spi_mode) != ERR_SUCCESS)
        {
            vSemaphoreDelete(sem);
            gpio_reset_to_defaults(mosi_gpio);
            gpio_reset_to_defaults(sck_gpio);
            gpio_reset_to_defaults(cs_gpio);
            if (ldac_en) gpio_reset_to_defaults(ldac_gpio);
            return ERR_INVALID_PARAMETER;
        }

        chip->periph          = periph;
        chip->clock_hz        = clock_hz;
        chip->spi_mode        = spi_mode;
        chip->mosi_pin        = mosi_gpio;
        chip->sck_pin         = sck_gpio;
        chip->cs_pin          = cs_gpio;
        chip->ldac_pin        = ldac_gpio;
        chip->ldac_enabled    = ldac_en;
        chip->tx_done         = sem;
        chip->channels_active = 0U;
        chip->initialized     = true;

        /* External reference: internal ref is powered down by default at chip
         * reset, so no command is needed. Only send the setup command when
         * enabling the internal reference. */
        if (reference != DAC_REFERENCE_EXTERNAL)
        {
            uint16_t ref_data = (reference == DAC_REFERENCE_INTERNAL_FLEX) ? DAC_REF_DATA_FLEX : DAC_REF_DATA_STATIC;
            build_frame(chip->tx_buf, DAC_CMD_REF_SETUP, DAC_ADDR_REF, ref_data);
            err_code_t werr = chip_write(chip);
            if (werr != ERR_SUCCESS)
            {
                chip_teardown(chip);
                return werr;
            }
        }
    }

    build_frame(chip->tx_buf, DAC_CMD_WRITE_AND_LOAD, channel, initial_value);
    err_code_t werr = chip_write(chip);
    if (werr != ERR_SUCCESS)
    {
        if (chip->channels_active == 0U)
            chip_teardown(chip);
        return werr;
    }

    chip->channels_active |= (1U << channel);
    states[idx].periph = periph;
    states[idx].channel = channel; 
    states[idx].in_use = true;

    uint8_t id = sensor_manager_register(PROTO_ID_DAC, idx);
    if (id == PROTO_SENSOR_ID_NONE)
    {
        chip->channels_active &= (uint8_t)~(1U << channel);
        states[idx].in_use = false;
        if (chip->channels_active == 0U)
            chip_teardown(chip);
        return ERR_OUT_OF_RESOURCES;
    }

    *out_sensor_id = id;
    return ERR_SUCCESS;
}

err_code_t dac_sensor_set_output(uint8_t internal_id, const uint8_t *values, uint16_t values_len)
{
    assert(values != NULL || values_len == 0U);

    if (internal_id >= DAC_SENSOR_MAX_COUNT) return ERR_INVALID_SENSOR_ID;

    dac_sensor_state_t *s = &states[internal_id];
    if (!s->in_use) return ERR_INVALID_SENSOR_ID;
    if (values_len < DAC_SET_OUTPUT_SIZE) return ERR_MALFORMED_PAYLOAD;

    uint16_t value = read_u16_le(&values[DAC_SET_OUTPUT_OFFSET_VALUE]);
    dac_chip_t *chip = &chips[s->periph];
    if (!chip->initialized) return ERR_INTERNAL;

    build_frame(chip->tx_buf, DAC_CMD_WRITE_AND_LOAD, s->channel, value);
    return chip_write(chip);
}

err_code_t dac_sensor_stop(uint8_t internal_id)
{
    if (internal_id >= DAC_SENSOR_MAX_COUNT) return ERR_INVALID_SENSOR_ID;

    dac_sensor_state_t *s = &states[internal_id];
    if (!s->in_use) return ERR_INVALID_SENSOR_ID;

    dac_chip_t *chip = &chips[s->periph];
    chip->channels_active &= (uint8_t)~(1U << s->channel);
    s->in_use = false;

    if (chip->channels_active == 0U)
        chip_teardown(chip);

    return ERR_SUCCESS;
}
