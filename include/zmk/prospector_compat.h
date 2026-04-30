/*
 * Prospector ZMK Module - Zephyr Version Compatibility
 *
 * Provides compatibility macros for building with both
 * Zephyr 3.5 (ZMK 0.3) and Zephyr 4.x (ZMK main).
 */

#ifndef ZMK_PROSPECTOR_COMPAT_H
#define ZMK_PROSPECTOR_COMPAT_H

#include <version.h>

/*
 * SYS_INIT function signature changed in Zephyr 4.0:
 *   Zephyr 3.x: int (*)(const struct device *dev)
 *   Zephyr 4.x: int (*)(void)
 *
 * Usage:
 *   static int my_init(PROSPECTOR_SYS_INIT_ARGS) {
 *       PROSPECTOR_SYS_INIT_UNUSED;
 *       ...
 *   }
 *   SYS_INIT(my_init, APPLICATION, 90);
 */
#if KERNEL_VERSION_MAJOR >= 4
#define PROSPECTOR_SYS_INIT_ARGS    void
#define PROSPECTOR_SYS_INIT_UNUSED  /* nothing */
#else
#define PROSPECTOR_SYS_INIT_ARGS    const struct device *dev
#define PROSPECTOR_SYS_INIT_UNUSED  ARG_UNUSED(dev)
#endif

#endif /* ZMK_PROSPECTOR_COMPAT_H */
