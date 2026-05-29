#include "spi_slave_sensor.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "err_codes.h"
#include "gpio.h"
#include "helpers.h"
#include "protocol.h"
#include "sensor_manager.h"
#include "spi_slave.h"

typedef struct
{
    bool         in_use;
    spi_periph_t periph;
    gpio_pin_t   miso_pin;
    gpio_pin_t   sck_pin;
    gpio_pin_t   nss_pin;
    gpio_pin_t   mosi_pin;
    bool         mosi_enabled;
} spi_slave_sensor_state_t;

static spi_slave_sensor_state_t states[SPI_SLAVE_SENSOR_MAX_COUNT];

static void sensor_teardown(spi_slave_sensor_state_t *s)
{
    (void)spi_slave_deinit(s->periph);
    gpio_reset_to_defaults(s->miso_pin);
    gpio_reset_to_defaults(s->sck_pin);
    gpio_reset_to_defaults(s->nss_pin);
    if (s->mosi_enabled)
        gpio_reset_to_defaults(s->mosi_pin);
    s->in_use = false;
}

void spi_slave_sensor_init(void)
{
    memset(states, 0, sizeof(states));
}

err_code_t spi_slave_sensor_setup(const uint8_t *cfg,
                                  uint16_t       cfg_len,
                                  uint8_t       *out_sensor_id)
{
    assert(cfg != NULL || cfg_len == 0U);
    assert(out_sensor_id != NULL);
    *out_sensor_id = PROTO_SENSOR_ID_NONE;

    if (cfg_len < SPI_SLAVE_CFG_MIN_SIZE)
        return ERR_MALFORMED_PAYLOAD;

    uint8_t  spi_periph_val = cfg[SPI_SLAVE_CFG_OFFSET_SPI_PERIPH];
    uint8_t  spi_mode_val   = cfg[SPI_SLAVE_CFG_OFFSET_SPI_MODE];
    uint8_t  miso_port     = cfg[SPI_SLAVE_CFG_OFFSET_MISO_PORT];
    uint8_t  miso_pin_val  = cfg[SPI_SLAVE_CFG_OFFSET_MISO_PIN];
    uint8_t  miso_af       = cfg[SPI_SLAVE_CFG_OFFSET_MISO_AF];
    uint8_t  sck_port      = cfg[SPI_SLAVE_CFG_OFFSET_SCK_PORT];
    uint8_t  sck_pin_val   = cfg[SPI_SLAVE_CFG_OFFSET_SCK_PIN];
    uint8_t  sck_af        = cfg[SPI_SLAVE_CFG_OFFSET_SCK_AF];
    uint8_t  nss_port      = cfg[SPI_SLAVE_CFG_OFFSET_NSS_PORT];
    uint8_t  nss_pin_val   = cfg[SPI_SLAVE_CFG_OFFSET_NSS_PIN];
    uint8_t  nss_af        = cfg[SPI_SLAVE_CFG_OFFSET_NSS_AF];
    uint8_t  mosi_port     = cfg[SPI_SLAVE_CFG_OFFSET_MOSI_PORT];
    uint8_t  mosi_pin_val  = cfg[SPI_SLAVE_CFG_OFFSET_MOSI_PIN];
    uint8_t  mosi_af       = cfg[SPI_SLAVE_CFG_OFFSET_MOSI_AF];
    uint16_t tx_buf_len    = read_u16_le(&cfg[SPI_SLAVE_CFG_OFFSET_TX_BUF_LEN]);

    bool mosi_en = (mosi_port != SPI_SLAVE_MOSI_PORT_DISABLED);

    if ((uint16_t)cfg_len < (uint16_t)(SPI_SLAVE_CFG_MIN_SIZE + tx_buf_len))
        return ERR_MALFORMED_PAYLOAD;

    if (spi_periph_val >= (uint8_t)SPI_PERIPH_COUNT ||
        spi_mode_val > (uint8_t)SPI_MODE_MAX ||
        miso_port > (uint8_t)GPIO_PORT_MAX || miso_pin_val > GPIO_PIN_MAX ||
        miso_af == 0U || miso_af > GPIO_AF_MAX ||
        sck_port > (uint8_t)GPIO_PORT_MAX || sck_pin_val > GPIO_PIN_MAX ||
        sck_af == 0U || sck_af > GPIO_AF_MAX ||
        nss_port > (uint8_t)GPIO_PORT_MAX || nss_pin_val > GPIO_PIN_MAX ||
        nss_af == 0U || nss_af > GPIO_AF_MAX ||
        (mosi_en && (mosi_port > (uint8_t)GPIO_PORT_MAX || mosi_pin_val > GPIO_PIN_MAX ||
                     mosi_af == 0U || mosi_af > GPIO_AF_MAX)) ||
         tx_buf_len == 0U || tx_buf_len > SPI_SLAVE_TX_BUF_MAX)
    {
        return ERR_INVALID_PARAMETER;
    }

    spi_periph_t periph   = (spi_periph_t)spi_periph_val;
    spi_mode_t   spi_mode = (spi_mode_t)spi_mode_val;
    uint8_t      idx      = (uint8_t)periph;

    if (states[idx].in_use)
        return ERR_PERIPHERAL_BUSY;

    /* All validation passed — configure GPIO pins then init SPI slave. */
    gpio_pin_t miso_gpio = gpio_make_pin((gpio_port_t)miso_port, miso_pin_val);
    gpio_pin_t sck_gpio  = gpio_make_pin((gpio_port_t)sck_port,  sck_pin_val);
    gpio_pin_t nss_gpio  = gpio_make_pin((gpio_port_t)nss_port,  nss_pin_val);
    gpio_pin_t mosi_gpio = {0};

    gpio_enable_clock((gpio_port_t)miso_port);
    gpio_af_config_t miso_cfg = { .pin = miso_gpio, .af = miso_af, .speed = GPIO_SPEED_HIGH, .pull = GPIO_PULL_NONE };
    gpio_configure_af(&miso_cfg);

    gpio_enable_clock((gpio_port_t)sck_port);
    gpio_af_config_t sck_cfg = { .pin = sck_gpio, .af = sck_af, .speed = GPIO_SPEED_HIGH, .pull = GPIO_PULL_NONE };
    gpio_configure_af(&sck_cfg);

    gpio_enable_clock((gpio_port_t)nss_port);
    gpio_af_config_t nss_cfg = { .pin = nss_gpio, .af = nss_af, .speed = GPIO_SPEED_HIGH, .pull = GPIO_PULL_NONE };
    gpio_configure_af(&nss_cfg);

    if (mosi_en)
    {
        mosi_gpio = gpio_make_pin((gpio_port_t)mosi_port, mosi_pin_val);
        gpio_enable_clock((gpio_port_t)mosi_port);
        gpio_af_config_t mosi_cfg = { .pin = mosi_gpio, .af = mosi_af, .speed = GPIO_SPEED_HIGH, .pull = GPIO_PULL_NONE };
        gpio_configure_af(&mosi_cfg);
    }

    /* Load the initial TX buffer into the driver before init so the
     * peripheral has data ready before the first NSS assertion. */
    const uint8_t *tx_data = &cfg[SPI_SLAVE_CFG_OFFSET_TX_BUF];

    err_code_t err = spi_slave_init(periph, spi_mode);
    if (err != ERR_SUCCESS)
    {
        gpio_reset_to_defaults(miso_gpio);
        gpio_reset_to_defaults(sck_gpio);
        gpio_reset_to_defaults(nss_gpio);
        if (mosi_en)
            gpio_reset_to_defaults(mosi_gpio);
        return err;
    }

    err = spi_slave_set_tx(periph, tx_data, (uint8_t)tx_buf_len);
    if (err != ERR_SUCCESS)
    {
        (void)spi_slave_deinit(periph);
        gpio_reset_to_defaults(miso_gpio);
        gpio_reset_to_defaults(sck_gpio);
        gpio_reset_to_defaults(nss_gpio);
        if (mosi_en)
            gpio_reset_to_defaults(mosi_gpio);
        return err;
    }

    states[idx].periph       = periph;
    states[idx].miso_pin     = miso_gpio;
    states[idx].sck_pin      = sck_gpio;
    states[idx].nss_pin      = nss_gpio;
    states[idx].mosi_pin     = mosi_gpio;
    states[idx].mosi_enabled = mosi_en;
    states[idx].in_use       = true;

    uint8_t id = sensor_manager_register(PROTO_ID_SPI, idx);
    if (id == PROTO_SENSOR_ID_NONE)
    {
        sensor_teardown(&states[idx]);
        return ERR_OUT_OF_RESOURCES;
    }

    *out_sensor_id = id;
    return ERR_SUCCESS;
}

err_code_t spi_slave_sensor_set_output(uint8_t        internal_id,
                                       const uint8_t *values,
                                       uint16_t       values_len)
{
    if (internal_id >= SPI_SLAVE_SENSOR_MAX_COUNT)
        return ERR_INVALID_SENSOR_ID;

    spi_slave_sensor_state_t *s = &states[internal_id];
    if (!s->in_use)
        return ERR_INVALID_SENSOR_ID;

    if (values_len < SPI_SLAVE_SET_OUTPUT_MIN_SIZE)
        return ERR_MALFORMED_PAYLOAD;

    uint16_t tx_buf_len = read_u16_le(&values[SPI_SLAVE_SET_OUTPUT_OFFSET_TX_BUF_LEN]);

    if ((uint16_t)values_len < (uint16_t)(SPI_SLAVE_SET_OUTPUT_MIN_SIZE + tx_buf_len))
        return ERR_MALFORMED_PAYLOAD;

    if (tx_buf_len == 0U || tx_buf_len > SPI_SLAVE_TX_BUF_MAX)
        return ERR_INVALID_PARAMETER;

    return spi_slave_set_tx(s->periph,
                            &values[SPI_SLAVE_SET_OUTPUT_OFFSET_TX_BUF],
                            (uint8_t)tx_buf_len);
}

err_code_t spi_slave_sensor_stop(uint8_t internal_id)
{
    if (internal_id >= SPI_SLAVE_SENSOR_MAX_COUNT)
        return ERR_INVALID_SENSOR_ID;

    spi_slave_sensor_state_t *s = &states[internal_id];
    if (!s->in_use)
        return ERR_INVALID_SENSOR_ID;

    sensor_teardown(s);
    return ERR_SUCCESS;
}
