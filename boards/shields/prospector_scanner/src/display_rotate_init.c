#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>

int disp_set_orientation(void)
{
	// Orientation is now handled by mdac=0x60 in Device Tree overlay
	// No additional software rotation needed to prevent display issues
	
	// const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	// if (!device_is_ready(display))
	// {
	// 	return -EIO;
	// }

	// Software rotation disabled - using hardware mdac=0x60 setting instead
	// #ifdef CONFIG_PROSPECTOR_ROTATE_DISPLAY_180
	// 	int ret = display_set_orientation(display, DISPLAY_ORIENTATION_ROTATED_90);
	// #else
	// 	int ret = display_set_orientation(display, DISPLAY_ORIENTATION_ROTATED_270);
	// #endif
	// if (ret < 0)
	// {
	// 	return ret;
	// }

	return 0;
}

SYS_INIT(disp_set_orientation, APPLICATION, 60);