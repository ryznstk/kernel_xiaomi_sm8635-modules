/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __XIAOMI__TOUCH_H
#define __XIAOMI__TOUCH_H

#include <linux/xiaomi_touch.h>

/*
 * Interface for interacting with Xiaomi touch functionality.
 * Provides methods to configure and query touch-related modes.
 */
struct xiaomi_touch_interface {
	/**
	 * set_mode_value: - Sets the value of a specified mode.
	 * @private: Private data from the touchscreen driver.
	 * @type: The mode to configure.
	 * @value: The value to assign to the mode (e.g., enable/disable or a parameter).
	 *
	 * Returns: 0 on success, or a negative error code on failure.
	 */
	int (*set_mode_value)(void *private, enum touch_mode mode, int value);

	/**
	 * get_mode_value: - Retrieves the current value of a specified mode.
	 * @private: Private data from the touchscreen driver.
	 * @type: The mode to query.
	 *
	 * Returns: The current value of the mode on success, or a negative error code on failure.
	 */
	int (*get_mode_value)(void *private, enum touch_mode mode);

	/*
	 * Private data from the touchscreen driver.
	 * This typically contains the core data necessary for the callbacks.
	 */
	void *private;
};

enum touch_id {
	TOUCH_ID_PRIMARY,
	TOUCH_ID_SECONDARY,
	TOUCH_ID_NUM,
};

/**
 * register_xiaomi_touch_client: - Register a client of xiaomi touch.
 * @touch_id: Type of the touchscreen this client belongs to.
 * @interface: Implemented interface providing the callbacks to get/set modes.
 *
 * Stores a pointer to the interface and associates the provided touch_id with it.
 * The interface must be held in memory until unregister_xiaomi_touch_client is called.
 */
int register_xiaomi_touch_client(enum touch_id touch_id,
				 struct xiaomi_touch_interface *interface);

/**
 * unregister_xiaomi_touch_client: - Unregister a client of xiaomi touch.
 * @touch_id: Type of the touchscreen whose client should be unregistered.
 */
int unregister_xiaomi_touch_client(enum touch_id touch_id);

enum touch_filter_type {
	TOUCH_FILTER_UP_THRESHOLD,
	TOUCH_FILTER_TOLERANCE,
	TOUCH_FILTER_AIM_SENSITIVITY,
	TOUCH_FILTER_TAP_STABILITY,
	TOUCH_FILTER_EDGE_FILTER,
	TOUCH_FILTER_PANEL_ORIENTATION,
	TOUCH_FILTER_EXPERT_MODE,
	TOUCH_FILTER_NUM,
};

static inline enum touch_filter_type touch_mode_to_filter(enum touch_mode mode)
{
	switch (mode) {
	case TOUCH_MODE_TOUCH_UP_THRESHOLD:
		return TOUCH_FILTER_UP_THRESHOLD;
	case TOUCH_MODE_TOUCH_TOLERANCE:
		return TOUCH_FILTER_TOLERANCE;
	case TOUCH_MODE_TOUCH_AIM_SENSITIVITY:
		return TOUCH_FILTER_AIM_SENSITIVITY;
	case TOUCH_MODE_TOUCH_TAP_STABILITY:
		return TOUCH_FILTER_TAP_STABILITY;
	case TOUCH_MODE_TOUCH_EDGE_FILTER:
		return TOUCH_FILTER_EDGE_FILTER;
	case TOUCH_MODE_PANEL_ORIENTATION:
		return TOUCH_FILTER_PANEL_ORIENTATION;
	case TOUCH_MODE_EXPERT_MODE:
		return TOUCH_FILTER_EXPERT_MODE;
	default:
		return TOUCH_FILTER_NUM;
	}
}

enum oneshot_sensor_type {
	ONESHOT_SENSOR_SINGLE_TAP,
	ONESHOT_SENSOR_DOUBLE_TAP,
	ONESHOT_SENSOR_FOD_PRESS,
	ONESHOT_SENSOR_TYPE_NUM,
};

/**
 * notify_oneshot_sensor: - Update the value of a oneshot sensor and notify user space.
 * @sensor_type: Which sensors value to update.
 * @value:       Sensor event value.
 *
 * Updates the pending_event value of a oneshot sensor and notifies
 * user space about it. When userspace reads the value, it will be
 * reset to 0. value is usually 0/1 to indicate that the event
 * happened or was revoked.
 */
int notify_oneshot_sensor(enum oneshot_sensor_type sensor_type, int value);

#endif
