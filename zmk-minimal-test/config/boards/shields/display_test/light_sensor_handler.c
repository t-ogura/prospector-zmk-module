/**
 * Light Sensor Handler - Test APDS9960 initialization
 * Tests if the sensor driver causes any issues (even without hardware)
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(light_sensor, LOG_LEVEL_INF);

/* Periodic sensor read work */
static void sensor_read_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sensor_read_work, sensor_read_work_handler);

static const struct device *light_dev = NULL;
static bool sensor_available = false;

static void sensor_read_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!sensor_available || light_dev == NULL) {
        return;
    }

    struct sensor_value light_val;
    int ret = sensor_sample_fetch_chan(light_dev, SENSOR_CHAN_LIGHT);
    if (ret == 0) {
        ret = sensor_channel_get(light_dev, SENSOR_CHAN_LIGHT, &light_val);
        if (ret == 0) {
            LOG_INF("Light: %d.%06d lux", light_val.val1, light_val.val2);
        }
    }

    /* Schedule next read in 5 seconds */
    k_work_schedule(&sensor_read_work, K_SECONDS(5));
}

static int light_sensor_init(void)
{
    LOG_INF("Light sensor init...");

    #if DT_NODE_EXISTS(DT_NODELABEL(apds9960))
    light_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(apds9960));

    if (light_dev == NULL) {
        LOG_WRN("APDS9960 device not found in DT");
        return 0;  /* Not an error - sensor is optional */
    }

    if (!device_is_ready(light_dev)) {
        LOG_WRN("APDS9960 device not ready (hardware not connected?)");
        light_dev = NULL;
        return 0;  /* Not an error - sensor is optional */
    }

    sensor_available = true;
    LOG_INF("APDS9960 light sensor ready");

    /* Start periodic reads */
    k_work_schedule(&sensor_read_work, K_SECONDS(2));
    #else
    LOG_INF("APDS9960 not configured in device tree");
    #endif

    return 0;
}

SYS_INIT(light_sensor_init, APPLICATION, 96);
