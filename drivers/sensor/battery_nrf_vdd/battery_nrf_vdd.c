/*
 * Battery gauge from the nRF52 SAADC VDD input, CR2032 curve.
 *
 * Shape follows ZMK's battery_nrf_vddh.c (nRF52840 VDDH/5), adapted to the
 * nRF52832 where the SAADC can sample VDD directly: gain 1/6 against the
 * 0.6 V internal reference puts 3.6 V at full scale, so no divider is needed.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT prospector_battery_nrf_vdd

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_saadc.h>

LOG_MODULE_REGISTER(battery_nrf_vdd, CONFIG_SENSOR_LOG_LEVEL);

static const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc));

struct vdd_data {
    struct adc_channel_cfg acc;
    struct adc_sequence as;
    uint16_t adc_raw;
    uint16_t millivolts;
    uint8_t state_of_charge;
};

/*
 * CR2032 under a light, mostly-idle load: flat around 2.9-3.0 V for most of
 * its life, a knee near 2.7 V, then a fast drop. First approximation - the
 * breakpoints are meant to be adjusted against measurements of this board.
 */
static const struct { uint16_t mv; uint8_t pct; } cr2032_curve[] = {
    { 3000, 100 }, { 2900, 80 }, { 2800, 55 }, { 2700, 30 },
    { 2600, 15 },  { 2500, 5 },  { 2300, 0 },
};

static uint8_t cr2032_mv_to_pct(uint16_t mv)
{
    const size_t n = ARRAY_SIZE(cr2032_curve);

    if (mv >= cr2032_curve[0].mv) {
        return 100;
    }
    for (size_t i = 1; i < n; i++) {
        if (mv >= cr2032_curve[i].mv) {
            /* linear between breakpoints i-1 (higher) and i (lower) */
            uint16_t hi_mv = cr2032_curve[i - 1].mv, lo_mv = cr2032_curve[i].mv;
            uint8_t hi_pct = cr2032_curve[i - 1].pct, lo_pct = cr2032_curve[i].pct;
            return lo_pct + (uint32_t)(mv - lo_mv) * (hi_pct - lo_pct) / (hi_mv - lo_mv);
        }
    }
    return 0;
}

static int vdd_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
    if (chan != SENSOR_CHAN_GAUGE_VOLTAGE && chan != SENSOR_CHAN_GAUGE_STATE_OF_CHARGE &&
        chan != SENSOR_CHAN_VOLTAGE && chan != SENSOR_CHAN_ALL) {
        return -ENOTSUP;
    }

    struct vdd_data *d = dev->data;
    int rc = adc_read(adc, &d->as);
    d->as.calibrate = false;
    if (rc != 0) {
        LOG_ERR("ADC read failed: %d", rc);
        return rc;
    }

    int32_t val = d->adc_raw;
    rc = adc_raw_to_millivolts(adc_ref_internal(adc), d->acc.gain, d->as.resolution, &val);
    if (rc != 0) {
        LOG_ERR("raw -> mV failed: %d", rc);
        return rc;
    }
    d->millivolts = (uint16_t)val;
    d->state_of_charge = cr2032_mv_to_pct(d->millivolts);
    LOG_DBG("VDD raw %u = %u mV -> %u%%", d->adc_raw, d->millivolts, d->state_of_charge);
    return 0;
}

static int vdd_channel_get(const struct device *dev, enum sensor_channel chan,
                           struct sensor_value *val)
{
    const struct vdd_data *d = dev->data;

    switch (chan) {
    case SENSOR_CHAN_GAUGE_VOLTAGE:
    case SENSOR_CHAN_VOLTAGE:
        val->val1 = d->millivolts / 1000;
        val->val2 = (d->millivolts % 1000) * 1000U;
        return 0;
    case SENSOR_CHAN_GAUGE_STATE_OF_CHARGE:
        val->val1 = d->state_of_charge;
        val->val2 = 0;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static const struct sensor_driver_api vdd_api = {
    .sample_fetch = vdd_sample_fetch,
    .channel_get = vdd_channel_get,
};

static int vdd_init(const struct device *dev)
{
    struct vdd_data *d = dev->data;

    if (!device_is_ready(adc)) {
        LOG_ERR("ADC %s not ready", adc->name);
        return -ENODEV;
    }

    d->as = (struct adc_sequence){
        .channels = BIT(0),
        .buffer = &d->adc_raw,
        .buffer_size = sizeof(d->adc_raw),
        .oversampling = 4,
        .calibrate = true,
        .resolution = 12,
    };
#ifdef CONFIG_ADC_NRFX_SAADC
    d->acc = (struct adc_channel_cfg){
        .gain = ADC_GAIN_1_6,
        .reference = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
        .input_positive = NRF_SAADC_INPUT_VDD,
    };
#else
#error "prospector,battery-nrf-vdd needs the nRF SAADC"
#endif

    int rc = adc_channel_setup(adc, &d->acc);
    LOG_INF("VDD gauge ready (SAADC VDD, 1/6 gain), setup=%d", rc);
    return rc;
}

static struct vdd_data vdd_data;

DEVICE_DT_INST_DEFINE(0, vdd_init, NULL, &vdd_data, NULL, POST_KERNEL,
                      CONFIG_SENSOR_INIT_PRIORITY, &vdd_api);
