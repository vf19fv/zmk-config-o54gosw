#define DT_DRV_COMPAT zmk_behavior_battery_lr_printer

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/battery.h>
#include <dt-bindings/zmk/keys.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Max produced string is "L100% R100% " (11 chars), keep extra room for safety. */
#define MAX_CHARS 24
#define TYPE_DELAY_MS 10
#define MAX_PERCENT_DIGITS 4

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct behavior_battery_lr_printer_data {
    struct k_work_delayable typing_work;
    uint8_t chars[MAX_CHARS];
    uint8_t chars_len;
    uint8_t current_idx;
    bool key_pressed;
};

static const uint32_t digit_keycodes[10] = {
    NUMBER_0, NUMBER_1, NUMBER_2, NUMBER_3, NUMBER_4,
    NUMBER_5, NUMBER_6, NUMBER_7, NUMBER_8, NUMBER_9
};

static void reset_typing_state(struct behavior_battery_lr_printer_data *data) {
    data->current_idx = 0;
    data->key_pressed = false;
    data->chars_len = 0;
    memset(data->chars, 0, sizeof(data->chars));
}

static bool append_char(struct behavior_battery_lr_printer_data *data, uint8_t ch) {
    if (data->chars_len >= ARRAY_SIZE(data->chars)) {
        return false;
    }
    data->chars[data->chars_len++] = ch;
    return true;
}

static uint32_t char_to_encoded_keycode(uint8_t ch) {
    if (ch >= '0' && ch <= '9') {
        return digit_keycodes[ch - '0'];
    }
    if (ch == 'L') {
        return L;
    }
    if (ch == 'R') {
        return R;
    }
    if (ch == '%') {
        return PERCENT;
    }
    if (ch == ' ') {
        return SPACE;
    }
    if (ch == '-') {
        return MINUS;
    }

    return 0;
}

static void uint_to_chars(uint32_t value, uint8_t *buffer, uint8_t *len) {
    char tmp[MAX_PERCENT_DIGITS];
    int count = 0;

    if (value == 0) {
        buffer[0] = '0';
        *len = 1;
        return;
    }

    while (value > 0 && count < (int)sizeof(tmp)) {
        tmp[count++] = '0' + (value % 10);
        value /= 10;
    }

    for (int i = 0; i < count; i++) {
        buffer[i] = tmp[count - 1 - i];
    }
    *len = count;
}

static bool append_percent_text(struct behavior_battery_lr_printer_data *data, char side, uint8_t percent) {
    if (!append_char(data, side)) {
        return false;
    }

    uint8_t digitbuf[MAX_PERCENT_DIGITS];
    uint8_t digitlen = 0;
    uint_to_chars(percent, digitbuf, &digitlen);
    for (int i = 0; i < digitlen; i++) {
        if (!append_char(data, digitbuf[i])) {
            return false;
        }
    }

    return append_char(data, '%');
}

static bool append_unknown_text(struct behavior_battery_lr_printer_data *data, char side) {
    return append_char(data, side) && append_char(data, '-') && append_char(data, '-') &&
           append_char(data, '%');
}

static void send_key(struct behavior_battery_lr_printer_data *data) {
    if (data->current_idx >= data->chars_len || data->current_idx >= ARRAY_SIZE(data->chars)) {
        reset_typing_state(data);
        return;
    }

    uint32_t keycode = char_to_encoded_keycode(data->chars[data->current_idx]);
    if (!keycode) {
        reset_typing_state(data);
        return;
    }

    bool pressed = !data->key_pressed;
    raise_zmk_keycode_state_changed_from_encoded(keycode, pressed, k_uptime_get());
    data->key_pressed = pressed;

    if (pressed) {
        k_work_schedule(&data->typing_work, K_MSEC(TYPE_DELAY_MS));
        return;
    }

    data->current_idx++;
    if (data->current_idx < data->chars_len) {
        k_work_schedule(&data->typing_work, K_MSEC(TYPE_DELAY_MS));
    } else {
        reset_typing_state(data);
    }
}

static void type_keys_work(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct behavior_battery_lr_printer_data *data =
        CONTAINER_OF(dwork, struct behavior_battery_lr_printer_data, typing_work);
    send_key(data);
}

static int behavior_battery_lr_printer_init(const struct device *dev) {
    struct behavior_battery_lr_printer_data *data = dev->data;
    k_work_init_delayable(&data->typing_work, type_keys_work);
    reset_typing_state(data);
    return 0;
}

static int on_pressed(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_battery_lr_printer_data *data = dev->data;

    if (data->key_pressed || data->current_idx) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    reset_typing_state(data);

    uint8_t left_percent = zmk_battery_state_of_charge();
    if (left_percent > 100) {
        left_percent = 100;
    }
    if (!append_percent_text(data, 'L', left_percent) || !append_char(data, ' ')) {
        reset_typing_state(data);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    uint8_t right_percent = 0;
    bool right_available = false;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    if (zmk_split_central_get_peripheral_battery_level(0, &right_percent) == 0) {
        right_available = true;
    }
#endif

    if (right_available) {
        if (right_percent > 100) {
            right_percent = 100;
        }
        if (!append_percent_text(data, 'R', right_percent)) {
            reset_typing_state(data);
            return ZMK_BEHAVIOR_OPAQUE;
        }
    } else {
        if (!append_unknown_text(data, 'R')) {
            reset_typing_state(data);
            return ZMK_BEHAVIOR_OPAQUE;
        }
    }

    if (!append_char(data, ' ')) {
        reset_typing_state(data);
        return ZMK_BEHAVIOR_OPAQUE;
    }
    send_key(data);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_released(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_battery_lr_printer_api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

#define BAT_INST(idx)                                                                              \
    static struct behavior_battery_lr_printer_data behavior_battery_lr_printer_data_##idx;        \
    BEHAVIOR_DT_INST_DEFINE(idx, behavior_battery_lr_printer_init, NULL,                          \
                            &behavior_battery_lr_printer_data_##idx, NULL,                        \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                            &behavior_battery_lr_printer_api);

DT_INST_FOREACH_STATUS_OKAY(BAT_INST)

#endif
