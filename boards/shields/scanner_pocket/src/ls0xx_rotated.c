/*
 * Copyright (c) 2020 Rohit Gujarathi
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sharp Memory LCD driver with 90° rotation support
 * Based on Zephyr's ls0xx.c driver
 *
 * When rotation is enabled:
 * - LVGL sees 168x144 (landscape)
 * - Hardware receives 144x168 (portrait, transposed)
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ls0xx_rotated, CONFIG_DISPLAY_LOG_LEVEL);

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/byteorder.h>

/* Only compile if scanner pocket shield is used */
#if defined(CONFIG_SHIELD_SCANNER_POCKET) && defined(CONFIG_SCANNER_POCKET_LANDSCAPE)

/* Physical display dimensions (hardware) */
#define LS0XX_HW_WIDTH   144
#define LS0XX_HW_HEIGHT  168

/* Logical display dimensions (after rotation) */
#define LS0XX_LOGICAL_WIDTH   LS0XX_HW_HEIGHT  /* 168 */
#define LS0XX_LOGICAL_HEIGHT  LS0XX_HW_WIDTH   /* 144 */

#define LS0XX_PIXELS_PER_BYTE  8U
#define LS0XX_HW_BYTES_PER_LINE  (LS0XX_HW_WIDTH / LS0XX_PIXELS_PER_BYTE)  /* 18 bytes */

#define LS0XX_BIT_WRITECMD    0x01
#define LS0XX_BIT_VCOM        0x02
#define LS0XX_BIT_CLEAR       0x04

/* Full frame buffer for rotation (144 * 168 / 8 = 3024 bytes) */
static uint8_t frame_buffer[LS0XX_HW_WIDTH * LS0XX_HW_HEIGHT / LS0XX_PIXELS_PER_BYTE];

/* Track which lines need updating */
static uint16_t dirty_line_start = LS0XX_HW_HEIGHT;
static uint16_t dirty_line_end = 0;

struct ls0xx_rotated_config {
	struct spi_dt_spec bus;
};

static int ls0xx_cmd(const struct device *dev, uint8_t *buf, uint8_t len)
{
	const struct ls0xx_rotated_config *config = dev->config;
	struct spi_buf cmd_buf = { .buf = buf, .len = len };
	struct spi_buf_set buf_set = { .buffers = &cmd_buf, .count = 1 };

	return spi_write_dt(&config->bus, &buf_set);
}

static int ls0xx_clear(const struct device *dev)
{
	const struct ls0xx_rotated_config *config = dev->config;
	uint8_t clear_cmd[2] = { LS0XX_BIT_CLEAR, 0 };
	int err;

	/* Clear frame buffer (0xFF = all white for MONO01) */
	memset(frame_buffer, 0xFF, sizeof(frame_buffer));
	dirty_line_start = LS0XX_HW_HEIGHT;
	dirty_line_end = 0;

	err = ls0xx_cmd(dev, clear_cmd, sizeof(clear_cmd));
	spi_release_dt(&config->bus);

	return err;
}

static int ls0xx_blanking_off(const struct device *dev)
{
	LOG_WRN("Blanking not supported");
	return -ENOTSUP;
}

static int ls0xx_blanking_on(const struct device *dev)
{
	LOG_WRN("Blanking not supported");
	return -ENOTSUP;
}

/*
 * Flush dirty lines from frame buffer to display
 */
static int ls0xx_flush_lines(const struct device *dev,
			     uint16_t start_line, uint16_t end_line)
{
	const struct ls0xx_rotated_config *config = dev->config;
	uint8_t write_cmd[1] = { LS0XX_BIT_WRITECMD };
	uint8_t dummy = 0;
	int err;

	if (start_line >= end_line) {
		return 0;
	}

	LOG_DBG("Flushing lines %d to %d", start_line, end_line - 1);

	err = ls0xx_cmd(dev, write_cmd, sizeof(write_cmd));
	if (err) {
		return err;
	}

	/* Send each line */
	for (uint16_t ln = start_line; ln < end_line; ln++) {
		uint8_t line_num = ln + 1;  /* Display uses 1-based line numbers */
		uint8_t *line_data = &frame_buffer[ln * LS0XX_HW_BYTES_PER_LINE];

		struct spi_buf line_buf[3] = {
			{ .len = 1, .buf = &line_num },
			{ .len = LS0XX_HW_BYTES_PER_LINE, .buf = line_data },
			{ .len = 1, .buf = &dummy },
		};
		struct spi_buf_set line_set = {
			.buffers = line_buf,
			.count = ARRAY_SIZE(line_buf),
		};

		err = spi_write_dt(&config->bus, &line_set);
		if (err) {
			break;
		}
	}

	/* Trailing dummy byte */
	err |= ls0xx_cmd(dev, &dummy, 1);

	spi_release_dt(&config->bus);

	return err;
}

/*
 * Set a pixel in the frame buffer (hardware coordinates)
 * x: 0-143 (hardware width)
 * y: 0-167 (hardware height)
 * value: 0 = black, 1 = white
 */
static inline void set_pixel_hw(uint16_t x, uint16_t y, uint8_t value)
{
	if (x >= LS0XX_HW_WIDTH || y >= LS0XX_HW_HEIGHT) {
		return;
	}

	uint32_t byte_idx = y * LS0XX_HW_BYTES_PER_LINE + x / 8;
	uint8_t bit_pos = x % 8;  /* LSB first for Sharp Memory LCD */

	if (value) {
		frame_buffer[byte_idx] |= (1 << bit_pos);
	} else {
		frame_buffer[byte_idx] &= ~(1 << bit_pos);
	}
}

/*
 * Write to display with 90° rotation
 *
 * Input coordinates (from LVGL): 168 wide x 144 tall
 * Output coordinates (hardware): 144 wide x 168 tall
 *
 * Rotation formula (90° clockwise):
 *   hw_x = logical_y
 *   hw_y = (LS0XX_LOGICAL_WIDTH - 1) - logical_x
 *
 * This means:
 *   - logical (0,0) -> hw (0, 167)  top-left -> bottom-left
 *   - logical (167,0) -> hw (0, 0)  top-right -> top-left
 *   - logical (0,143) -> hw (143, 167)  bottom-left -> bottom-right
 *   - logical (167,143) -> hw (143, 0)  bottom-right -> top-right
 */
static int ls0xx_write(const struct device *dev, const uint16_t x,
		       const uint16_t y,
		       const struct display_buffer_descriptor *desc,
		       const void *buf)
{
	const uint8_t *src = buf;

	LOG_DBG("Write: x=%d, y=%d, w=%d, h=%d", x, y, desc->width, desc->height);

	if (buf == NULL) {
		LOG_WRN("Display buffer is not available");
		return -EINVAL;
	}

	/* Validate bounds (logical coordinates) */
	if ((x + desc->width) > LS0XX_LOGICAL_WIDTH ||
	    (y + desc->height) > LS0XX_LOGICAL_HEIGHT) {
		LOG_ERR("Buffer out of bounds: x=%d+%d > %d or y=%d+%d > %d",
			x, desc->width, LS0XX_LOGICAL_WIDTH,
			y, desc->height, LS0XX_LOGICAL_HEIGHT);
		return -EINVAL;
	}

	/* Calculate pitch (bytes per row in source buffer) */
	uint16_t pitch = desc->pitch;
	if (pitch == 0) {
		pitch = desc->width;
	}
	uint16_t src_bytes_per_row = (pitch + 7) / 8;

	/* Copy pixels with rotation */
	for (uint16_t ly = 0; ly < desc->height; ly++) {
		for (uint16_t lx = 0; lx < desc->width; lx++) {
			/* Get pixel from source (MONO01: 1=white, 0=black) */
			/* Data from lvgl_transform_buffer is LSB first */
			uint16_t src_x = lx;
			uint16_t src_y = ly;
			uint32_t src_byte_idx = src_y * src_bytes_per_row + src_x / 8;
			uint8_t src_bit = src_x % 8;  /* LSB first from LVGL transform */
			uint8_t pixel = (src[src_byte_idx] >> src_bit) & 1;

			/* Calculate logical position */
			uint16_t logical_x = x + lx;
			uint16_t logical_y = y + ly;

			/* Rotate 90° clockwise: hw_x = ly, hw_y = (W-1) - lx */
			uint16_t hw_x = logical_y;
			uint16_t hw_y = (LS0XX_LOGICAL_WIDTH - 1) - logical_x;

			set_pixel_hw(hw_x, hw_y, pixel);

			/* Track dirty lines */
			if (hw_y < dirty_line_start) {
				dirty_line_start = hw_y;
			}
			if (hw_y >= dirty_line_end) {
				dirty_line_end = hw_y + 1;
			}
		}
	}

	/* Flush dirty region to display */
	int err = ls0xx_flush_lines(dev, dirty_line_start, dirty_line_end);

	/* Reset dirty tracking */
	dirty_line_start = LS0XX_HW_HEIGHT;
	dirty_line_end = 0;

	return err;
}

static void ls0xx_get_capabilities(const struct device *dev,
				   struct display_capabilities *caps)
{
	memset(caps, 0, sizeof(struct display_capabilities));
	/* Report rotated (logical) dimensions to LVGL */
	caps->x_resolution = LS0XX_LOGICAL_WIDTH;   /* 168 */
	caps->y_resolution = LS0XX_LOGICAL_HEIGHT;  /* 144 */
	caps->supported_pixel_formats = PIXEL_FORMAT_MONO01;
	caps->current_pixel_format = PIXEL_FORMAT_MONO01;
	caps->screen_info = 0;  /* No special alignment needed with full buffer */
}

static int ls0xx_set_pixel_format(const struct device *dev,
				  const enum display_pixel_format pf)
{
	if (pf == PIXEL_FORMAT_MONO01) {
		return 0;
	}

	LOG_ERR("Pixel format not supported");
	return -ENOTSUP;
}

static int ls0xx_rotated_init(const struct device *dev)
{
	const struct ls0xx_rotated_config *config = dev->config;

	if (!spi_is_ready_dt(&config->bus)) {
		LOG_ERR("SPI bus %s not ready", config->bus.bus->name);
		return -ENODEV;
	}

	LOG_INF("Sharp Memory LCD initialized with 90° rotation (168x144)");

	/* Clear display */
	return ls0xx_clear(dev);
}

/* Get SPI configuration from device tree */
#define DT_DRV_COMPAT sharp_ls0xx

static const struct ls0xx_rotated_config ls0xx_rotated_config = {
	.bus = SPI_DT_SPEC_INST_GET(
		0, SPI_OP_MODE_MASTER | SPI_WORD_SET(8) |
		SPI_TRANSFER_LSB | SPI_CS_ACTIVE_HIGH |
		SPI_HOLD_ON_CS | SPI_LOCK_ON, 0),
};

static DEVICE_API(display, ls0xx_rotated_driver_api) = {
	.blanking_on = ls0xx_blanking_on,
	.blanking_off = ls0xx_blanking_off,
	.write = ls0xx_write,
	.get_capabilities = ls0xx_get_capabilities,
	.set_pixel_format = ls0xx_set_pixel_format,
};

/* Override the default ls0xx driver with our rotated version */
DEVICE_DT_INST_DEFINE(0, ls0xx_rotated_init, NULL, NULL, &ls0xx_rotated_config,
		      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY,
		      &ls0xx_rotated_driver_api);

#endif /* CONFIG_SHIELD_SCANNER_POCKET && CONFIG_SCANNER_POCKET_LANDSCAPE */
