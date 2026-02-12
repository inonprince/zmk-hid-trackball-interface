/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_hid_trackball_interface

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/input/input.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/activity.h>

#if IS_ENABLED(CONFIG_ZMK_HID_TRACKBALL_INTERFACE_FEATURE_CHANNEL)
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define LED_SLCK 0x04

enum interface_input_mode {
    MOVE,
    SCROLL,
    SNIPE
};

struct interface_config {
    int32_t *scroll_layers;
    int scroll_layers_len;
    int32_t *snipe_layers;
    int snipe_layers_len;
    int32_t automouse_layer;
    int automouse_layer_timeout_ms;
};

struct interface_data {
    const struct device *dev;

    enum interface_input_mode curr_mode;
    bool automouse_enabled;
    struct k_work_delayable activate_automouse_layer_delayed;
    struct k_work_delayable deactivate_automouse_layer_delayed;
};

static int32_t scroll_layers[] = DT_PROP(DT_DRV_INST(0), scroll_layers);
static int32_t snipe_layers[] = DT_PROP(DT_DRV_INST(0), snipe_layers);

static const struct interface_config config = {
    .scroll_layers = scroll_layers,
    .scroll_layers_len = DT_PROP_LEN(DT_DRV_INST(0), scroll_layers),
    .snipe_layers = snipe_layers,
    .snipe_layers_len = DT_PROP_LEN(DT_DRV_INST(0), snipe_layers),
    .automouse_layer = DT_PROP(DT_DRV_INST(0), automouse_layer),
    .automouse_layer_timeout_ms = DT_PROP(DT_DRV_INST(0), automouse_layer_timeout_ms),
};

static struct interface_data data = {
    .dev = DEVICE_DT_INST_GET(0),
};

static void toggle_scroll() {
    struct zmk_behavior_binding binding = {
        .behavior_dev = DEVICE_DT_NAME(DT_PHANDLE(DT_DRV_INST(0), tog_scroll_bindings)),
    };
    zmk_behavior_queue_add(-1, binding, true, 0);
    LOG_INF("scroll toggled"); 
}

static void cycle_dpi() {
    struct zmk_behavior_binding binding = {
        .behavior_dev = DEVICE_DT_NAME(DT_PHANDLE(DT_DRV_INST(0), cyc_dpi_bindings)),
    };
    zmk_behavior_queue_add(-1, binding, true, 0);
    LOG_INF("cycle dpi");
}

static void activate_automouse_layer_work(struct k_work *item) {
    zmk_keymap_layer_activate(config.automouse_layer);
    LOG_INF("mouse layer activated (after idle wake)");
    data.automouse_enabled = true;
}

static void activate_automouse_layer() {
    if (zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE) {
        // Keyboard is idle/sleeping. Emit synthetic input event to wake it,
        // then delay layer activation so activity system processes first.
        input_report_rel(data.dev, INPUT_REL_MISC, 1, true, K_NO_WAIT);
        k_work_schedule(&data.activate_automouse_layer_delayed, K_MSEC(50));
        LOG_INF("waking from idle, delaying automouse activation");
    } else {
        zmk_keymap_layer_activate(config.automouse_layer);
        LOG_INF("mouse layer activated");
        data.automouse_enabled = true;
    }
}

static void deactivate_automouse_layer(struct k_work *item) {
    if (zmk_keymap_layer_active(config.automouse_layer)) {
        zmk_keymap_layer_deactivate(config.automouse_layer);
        LOG_INF("mouse layer deactivated");
    }
    data.automouse_enabled = false;
}

static int hid_indicators_listener_cb(const zmk_event_t *eh) {
    struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev->indicators & LED_SLCK) {
        if (!data.automouse_enabled && !zmk_keymap_layer_active(config.automouse_layer)) {
            activate_automouse_layer();
        } else if (k_work_delayable_is_pending(&data.deactivate_automouse_layer_delayed)) {
            k_work_cancel_delayable(&data.deactivate_automouse_layer_delayed);
        }
    } else if (data.automouse_enabled && !(ev->indicators & LED_SLCK)) {
        k_work_reschedule(&data.deactivate_automouse_layer_delayed, K_MSEC(config.automouse_layer_timeout_ms));
    }
    return 0;
}

ZMK_LISTENER(hid_indicators_listener, hid_indicators_listener_cb);
ZMK_SUBSCRIPTION(hid_indicators_listener, zmk_hid_indicators_changed);

static enum interface_input_mode get_input_mode_for_current_layer() {
    for (int i = 0; i < config.scroll_layers_len; i++) {
        if (zmk_keymap_layer_active(config.scroll_layers[i])) {
            return SCROLL;
        }
    }
    for (int i = 0; i < config.snipe_layers_len; i++) {
        if (zmk_keymap_layer_active(config.snipe_layers[i])) {
            return SNIPE;
        }
    }
    return MOVE;
}

static int layer_state_listener_cb(const zmk_event_t *eh) {
    enum interface_input_mode input_mode = get_input_mode_for_current_layer();
    if (input_mode != data.curr_mode) {
        LOG_INF("input mode changed to %d", input_mode);

        switch (input_mode) {
            case MOVE:
                if (data.curr_mode == SCROLL) {
                    toggle_scroll();
                } else if (data.curr_mode == SNIPE) {
                    cycle_dpi();
                }
                break;
            case SCROLL:
                if (data.curr_mode == MOVE) {
                    toggle_scroll();
                } else if (data.curr_mode == SNIPE) {
                    cycle_dpi();
                    toggle_scroll();
                }
                break;
            case SNIPE:
                if (data.curr_mode == MOVE) {
                    cycle_dpi();
                } else if (data.curr_mode == SCROLL) {
                    toggle_scroll();
                    cycle_dpi();
                }
                break;
        }
        data.curr_mode = input_mode;
    }
    return 0;
}

ZMK_LISTENER(layer_state_listener, layer_state_listener_cb);
ZMK_SUBSCRIPTION(layer_state_listener, zmk_layer_state_changed);

static int interface_init(const struct device *dev) {
    struct interface_data *data = dev->data;

    k_work_init_delayable(&data->activate_automouse_layer_delayed, activate_automouse_layer_work);
    k_work_init_delayable(&data->deactivate_automouse_layer_delayed, deactivate_automouse_layer);

    return 0;
}

DEVICE_DT_INST_DEFINE(0, &interface_init, NULL, &data, &config, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);

#if IS_ENABLED(CONFIG_ZMK_HID_TRACKBALL_INTERFACE_FEATURE_CHANNEL)

static const uint8_t vendor_hid_report_desc[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,        // Usage (Vendor Usage 1)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x01,        //   Report Count (1)
    0xB1, 0x02,        //   Feature (Data, Variable, Absolute)
    0xC0,              // End Collection
};

static const struct device *vendor_hid_dev;

static int vendor_set_report_cb(const struct device *dev,
                                struct usb_setup_packet *setup,
                                int32_t *len, uint8_t **buf) {
    if (*len < 2) {
        return -EINVAL;
    }
    uint8_t report_data = (*buf)[1]; // byte after report ID

    if (report_data & LED_SLCK) {
        if (!data.automouse_enabled && !zmk_keymap_layer_active(config.automouse_layer)) {
            activate_automouse_layer();
        } else if (k_work_delayable_is_pending(&data.deactivate_automouse_layer_delayed)) {
            k_work_cancel_delayable(&data.deactivate_automouse_layer_delayed);
        }
    } else if (data.automouse_enabled) {
        k_work_reschedule(&data.deactivate_automouse_layer_delayed,
                          K_MSEC(config.automouse_layer_timeout_ms));
    }
    return 0;
}

static const struct hid_ops vendor_ops = {
    .set_report = vendor_set_report_cb,
};

static int vendor_hid_init(void) {
    vendor_hid_dev = device_get_binding("HID_1");
    if (!vendor_hid_dev) {
        LOG_ERR("Cannot find HID_1 device");
        return -ENODEV;
    }
    usb_hid_register_device(vendor_hid_dev, vendor_hid_report_desc,
                            sizeof(vendor_hid_report_desc), &vendor_ops);
    usb_hid_init(vendor_hid_dev);
    return 0;
}

SYS_INIT(vendor_hid_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_HID_TRACKBALL_INTERFACE_FEATURE_CHANNEL */

#endif
