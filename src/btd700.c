#include "btd700/btd700_c.h"
#include "protocol_impl.h"

#include <hidapi/hidapi.h>
#include <stdlib.h>
#include <string.h>

struct btd700_driver {
    hid_device*             handle;
    btd700_event_callback_t event_cb;
    void*                   event_user_data;
};

static btd700_error_t hid_open_device(btd700_driver_t* drv) {
    struct hid_device_info* devs = hid_enumerate(BTD700_VENDOR_ID, BTD700_PID);
    struct hid_device_info* cur = devs;

    while (cur) {
        if (cur->usage_page == BTD700_USAGE_PAGE && cur->usage == BTD700_USAGE) {
            drv->handle = hid_open_path(cur->path);
            if (drv->handle) {
                hid_set_nonblocking(drv->handle, 1);
                hid_free_enumeration(devs);
                return BTD700_OK;
            }
        }
        cur = cur->next;
    }

    hid_free_enumeration(devs);
    return BTD700_ERR_DEVICE_NOT_FOUND;
}

static void dispatch_event(const btd700_driver_t* drv, const uint8_t* data, const int len) {
    if (!drv->event_cb || len < 4) return;

    btd700_event_type_t type;
    switch (data[2]) {
        case EVT_DONGLE_STATE:   type = BTD700_EVENT_STATE_CHANGED; break;
        case EVT_AUDIO_MODE:     type = BTD700_EVENT_AUDIO_MODE_CHANGED; break;
        case EVT_CODEC_TO_USE:   type = BTD700_EVENT_CODEC_CHANGED; break;
        case EVT_LE_AUDIO_STATE: type = BTD700_EVENT_LE_AUDIO_STATE_CHANGED; break;
        case EVT_AUDIO_QUALITY:  type = BTD700_EVENT_AUDIO_QUALITY_CHANGED; break;
        case EVT_SINK_TRANSPORT: type = BTD700_EVENT_SINK_TRANSPORT_CHANGED; break;
        case EVT_GAMING_STATUS:  type = BTD700_EVENT_GAMING_AVAILABILITY_CHANGED; break;
        default: return;
    }

    size_t data_len = (len > 4) ? (size_t)(len - 4) : 0;
    if (data_len > data[3]) data_len = data[3];

    const btd700_event_t event = { type, &data[4], data_len };
    drv->event_cb(&event, drv->event_user_data);
}

static btd700_error_t send_and_receive(const btd700_driver_t* drv,
                                        const uint8_t cmd,
                                        const uint8_t* args, const size_t args_len,
                                        uint8_t* resp, size_t* resp_len) {
    uint8_t packet[64];
    uint8_t buf[64];

    if (!drv->handle) return BTD700_ERR_DEVICE_NOT_OPEN;

    memset(packet, 0, sizeof(packet));
    packet[0] = BTD700_REPORT_ID;
    packet[1] = BTD700_MARKER_HOST_CMD;
    packet[2] = cmd;
    packet[3] = (uint8_t)args_len;
    if (args && args_len > 0) {
        memcpy(&packet[4], args, args_len < 60 ? args_len : 60);
    }

    if (hid_write(drv->handle, packet, sizeof(packet)) < 0) {
        return BTD700_ERR_HID;
    }

    for (int attempts = 0; attempts < 10; attempts++) {
        const int res = hid_read_timeout(drv->handle, buf, sizeof(buf), 100);

        if (res < 0) return BTD700_ERR_HID;
        if (res == 0) continue;

        if (res >= 4 && buf[0] == BTD700_REPORT_ID) {
            if (buf[1] == BTD700_MARKER_DONGLE_RSP && buf[2] == cmd) {
                if (resp && resp_len) {
                    size_t copy = (size_t)res < *resp_len ? (size_t)res : *resp_len;
                    memcpy(resp, buf, copy);
                    *resp_len = (size_t)res;
                }
                return BTD700_OK;
            }

            if (buf[1] == BTD700_MARKER_DONGLE_CMD) {
                dispatch_event(drv, buf, res);
            }
        }
    }

    return BTD700_ERR_HID;
}

// ==========================================================================================
// features
// ==========================================================================================

btd700_error_t btd700_driver_create(btd700_driver_t** out) {
    if (!out) return BTD700_ERR_INVALID_ARG;

    btd700_driver_t* drv = calloc(1, sizeof(btd700_driver_t));
    if (!drv) return BTD700_ERR_HID;

    if (hid_init() != 0) {
        free(drv);
        return BTD700_ERR_HID;
    }

    *out = drv;
    return BTD700_OK;
}

void btd700_driver_destroy(btd700_driver_t* drv) {
    if (!drv) return;
    if (drv->handle) {
        hid_close(drv->handle);
    }
    hid_exit();
    free(drv);
}

btd700_error_t btd700_driver_connect(btd700_driver_t* drv) {
    if (!drv) return BTD700_ERR_INVALID_ARG;
    if (drv->handle) return BTD700_OK;
    return hid_open_device(drv);
}

btd700_error_t btd700_driver_disconnect(btd700_driver_t* drv) {
    if (!drv) return BTD700_ERR_INVALID_ARG;
    if (drv->handle) {
        hid_close(drv->handle);
        drv->handle = NULL;
    }
    return BTD700_OK;
}

int btd700_driver_is_connected(btd700_driver_t* drv) {
    if (!drv) return 0;
    return drv->handle != NULL ? 1 : 0;
}

btd700_error_t btd700_driver_device_info(btd700_driver_t* drv,
                                          btd700_device_info_t* out) {
    if (!drv || !out) return BTD700_ERR_INVALID_ARG;
    if (!drv->handle) return BTD700_ERR_DEVICE_NOT_OPEN;

    wchar_t wbuf[256];
    memset(out, 0, sizeof(*out));

    if (hid_get_manufacturer_string(drv->handle, wbuf, 256) == 0) {
        wcstombs(out->manufacturer, wbuf, sizeof(out->manufacturer) - 1);
    }
    if (hid_get_product_string(drv->handle, wbuf, 256) == 0) {
        wcstombs(out->product, wbuf, sizeof(out->product) - 1);
    }
    if (hid_get_serial_number_string(drv->handle, wbuf, 256) == 0) {
        wcstombs(out->serial, wbuf, sizeof(out->serial) - 1);
    }

    return BTD700_OK;
}

btd700_error_t btd700_driver_firmware_version(btd700_driver_t* drv,
                                               btd700_firmware_version_t* out) {
    if (!drv || !out) return BTD700_ERR_INVALID_ARG;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    const btd700_error_t err = send_and_receive(drv, CMD_GET_FIRMWARE_VERSION, NULL, 0, resp, &resp_len);
    if (err != BTD700_OK) return err;

    memset(out, 0, sizeof(*out));
    if (resp_len >= 8) {
        out->major = resp[4];
        out->minor = resp[5];
        out->build = (uint16_t)(resp[6] | (resp[7] << 8));
    }
    return BTD700_OK;
}

btd700_error_t btd700_driver_state(btd700_driver_t* drv,
                                    btd700_dongle_state_t* out) {
    if (!drv || !out) return BTD700_ERR_INVALID_ARG;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    const btd700_error_t err = send_and_receive(drv, CMD_GET_DONGLE_STATE, NULL, 0, resp, &resp_len);
    if (err != BTD700_OK) return err;

    *out = BTD700_STATE_NONE;
    if (resp_len >= 5) {
        *out = (btd700_dongle_state_t)resp[4];
    }
    return BTD700_OK;
}

btd700_error_t btd700_driver_audio_config(btd700_driver_t* drv,
                                           btd700_audio_config_t* out) {
    if (!drv || !out) return BTD700_ERR_INVALID_ARG;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    const btd700_error_t err = send_and_receive(drv, CMD_GET_AUDIO_MODE, NULL, 0, resp, &resp_len);
    if (err != BTD700_OK) return err;

    memset(out, 0, sizeof(*out));
    if (resp_len >= 6) {
        out->mode = (btd700_audio_mode_t)resp[4];
        out->transport = (btd700_transport_mode_t)resp[5];
    }
    return BTD700_OK;
}

btd700_error_t btd700_driver_supported_codecs(btd700_driver_t* drv,
                                               uint16_t* out) {
    if (!drv || !out) return BTD700_ERR_INVALID_ARG;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    const btd700_error_t err = send_and_receive(drv, CMD_GET_SUPPORTED_CODEC, NULL, 0, resp, &resp_len);
    if (err != BTD700_OK) return err;

    *out = 0;
    if (resp_len >= 6) {
        *out = (uint16_t)(resp[4] | (resp[5] << 8));
    }
    return BTD700_OK;
}

btd700_error_t btd700_driver_active_codec(btd700_driver_t* drv,
                                           uint16_t* out) {
    if (!drv || !out) return BTD700_ERR_INVALID_ARG;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    const btd700_error_t err = send_and_receive(drv, CMD_GET_CODEC_IN_USE, NULL, 0, resp, &resp_len);
    if (err != BTD700_OK) return err;

    *out = 0;
    if (resp_len >= 6) {
        *out = (uint16_t)(resp[4] | (resp[5] << 8));
    }
    return BTD700_OK;
}

btd700_error_t btd700_driver_le_audio_state(btd700_driver_t* drv,
                                             btd700_le_audio_state_t* out) {
    if (!drv || !out) return BTD700_ERR_INVALID_ARG;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    const btd700_error_t err = send_and_receive(drv, CMD_GET_LE_AUDIO_STATE, NULL, 0, resp, &resp_len);
    if (err != BTD700_OK) return err;

    *out = BTD700_LE_AUDIO_NONE;
    if (resp_len >= 5) {
        *out = (btd700_le_audio_state_t)resp[4];
    }
    return BTD700_OK;
}

btd700_error_t btd700_driver_audio_quality(btd700_driver_t* drv,
                                            btd700_audio_quality_t* out) {
    if (!drv || !out) return BTD700_ERR_INVALID_ARG;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    const btd700_error_t err = send_and_receive(drv, CMD_GET_AUDIO_QUALITY, NULL, 0, resp, &resp_len);
    if (err != BTD700_OK) return err;

    memset(out, 0, sizeof(*out));
    if (resp_len >= 7) {
        out->resolution = (btd700_audio_resolution_t)resp[4];
        out->frequency = (btd700_audio_frequency_t)resp[5];
    }
    return BTD700_OK;
}

btd700_error_t btd700_driver_sink_transport(btd700_driver_t* drv,
                                             btd700_sink_mode_t* out) {
    if (!drv || !out) return BTD700_ERR_INVALID_ARG;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    const btd700_error_t err = send_and_receive(drv, CMD_GET_SINK_TRANSPORT, NULL, 0, resp, &resp_len);
    if (err != BTD700_OK) return err;

    *out = BTD700_SINK_NOT_AVAILABLE;
    if (resp_len >= 5) {
        *out = (btd700_sink_mode_t)resp[4];
    }
    return BTD700_OK;
}

// TODO: analyze this, for some reason it always returns an error to me
btd700_error_t btd700_driver_is_gaming_available(btd700_driver_t* drv, int* out) {
    if (!drv || !out) return BTD700_ERR_INVALID_ARG;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    const btd700_error_t err = send_and_receive(drv, CMD_GET_GAMING_STATUS, NULL, 0, resp, &resp_len);
    if (err != BTD700_OK) return err;

    *out = (resp_len >= 5 && resp[4] != 0) ? 1 : 0;
    return BTD700_OK;
}

btd700_error_t btd700_driver_broadcast_info(btd700_driver_t* drv,
                                             btd700_broadcast_info_t* out) {
    if (!drv || !out) return BTD700_ERR_INVALID_ARG;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    const btd700_error_t err = send_and_receive(drv, CMD_GET_BROADCAST_INFO, NULL, 0, resp, &resp_len);
    if (err != BTD700_OK) return err;

    memset(out, 0, sizeof(*out));
    if (resp_len >= 7) {
        out->state = (btd700_broadcast_state_t)resp[4];
        out->quality = (btd700_broadcast_quality_t)resp[5];
        out->encryption = (btd700_broadcast_encryption_t)resp[6];
    }
    return BTD700_OK;
}

btd700_error_t btd700_driver_broadcast_name(btd700_driver_t* drv,
                                             char* buf, const size_t buf_size) {
    if (!drv || !buf || buf_size == 0) return BTD700_ERR_INVALID_ARG;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    const btd700_error_t err = send_and_receive(drv, CMD_GET_BROADCAST_NAME, NULL, 0, resp, &resp_len);
    if (err != BTD700_OK) return err;

    buf[0] = '\0';
    if (resp_len > 4) {
        size_t j = 0;
        for (size_t i = 4; i < resp_len && resp[i] != 0 && j + 1 < buf_size; i++) {
            buf[j++] = (char)resp[i];
        }
        buf[j] = '\0';
    }
    return BTD700_OK;
}

btd700_error_t btd700_driver_broadcast_key(btd700_driver_t* drv,
                                            uint8_t* buf, const size_t buf_size,
                                            size_t* out_len) {
    if (!drv || !buf || buf_size == 0 || !out_len) return BTD700_ERR_INVALID_ARG;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    const btd700_error_t err = send_and_receive(drv, CMD_GET_BROADCAST_KEY, NULL, 0, resp, &resp_len);
    if (err != BTD700_OK) return err;

    if (resp_len > 4) {
        const size_t key_len = resp_len - 4;
        *out_len = key_len;
        const size_t copy_len = key_len < buf_size ? key_len : buf_size;
        memcpy(buf, &resp[4], copy_len);
    } else {
        *out_len = 0;
    }
    return BTD700_OK;
}

btd700_error_t btd700_driver_set_audio_mode(btd700_driver_t* drv,
                                             const btd700_audio_mode_t mode,
                                             const btd700_transport_mode_t transport) {
    if (!drv) return BTD700_ERR_INVALID_ARG;

    uint8_t args[2];
    args[0] = (uint8_t)mode;
    args[1] = (uint8_t)transport;

    return send_and_receive(drv, CMD_SET_AUDIO_MODE, args, 2, NULL, NULL);
}

btd700_error_t btd700_driver_set_codec_mask(btd700_driver_t* drv,
                                             const uint16_t codec_mask) {
    if (!drv) return BTD700_ERR_INVALID_ARG;

    uint8_t args[2];
    args[0] = (uint8_t)(codec_mask & 0xFF);
    args[1] = (uint8_t)((codec_mask >> 8) & 0xFF);

    return send_and_receive(drv, CMD_SET_CODEC, args, 2, NULL, NULL);
}

btd700_error_t btd700_driver_set_codec(btd700_driver_t* drv,
                                        const btd700_codec_t codec) {
    if (!drv) return BTD700_ERR_INVALID_ARG;

    const uint16_t mask = (uint16_t)(1 << (uint8_t)codec);
    return btd700_driver_set_codec_mask(drv, mask);
}

btd700_error_t btd700_driver_set_broadcast_info(btd700_driver_t* drv,
                                                 const btd700_broadcast_state_t state,
                                                 const btd700_broadcast_encryption_t encryption,
                                                 const btd700_broadcast_quality_t quality) {
    if (!drv) return BTD700_ERR_INVALID_ARG;

    uint8_t args[3];
    args[0] = (uint8_t)state;
    args[1] = (uint8_t)quality;
    args[2] = (uint8_t)encryption;

    return send_and_receive(drv, CMD_SET_BROADCAST_INFO, args, 3, NULL, NULL);
}

btd700_error_t btd700_driver_set_broadcast_name(btd700_driver_t* drv,
                                                 const char* name) {
    if (!drv || !name) return BTD700_ERR_INVALID_ARG;

    uint8_t args[61];
    size_t len = strlen(name);
    if (len + 1 > sizeof(args)) len = sizeof(args) - 1;

    memcpy(args, name, len);
    args[len] = 0;
    return send_and_receive(drv, CMD_SET_BROADCAST_NAME, args, len + 1, NULL, NULL);
}

btd700_error_t btd700_driver_set_broadcast_key(btd700_driver_t* drv,
                                                const uint8_t* key, size_t key_len) {
    if (!drv || (!key && key_len > 0)) return BTD700_ERR_INVALID_ARG;
    if (key_len > 61) key_len = 61;
    return send_and_receive(drv, CMD_SET_BROADCAST_KEY, key, key_len, NULL, NULL);
}

btd700_error_t btd700_driver_trigger_connect(btd700_driver_t* drv) {
    if (!drv) return BTD700_ERR_INVALID_ARG;
    const uint8_t arg = 1;
    return send_and_receive(drv, CMD_SET_BT_CONNECT, &arg, 1, NULL, NULL);
}

btd700_error_t btd700_driver_trigger_disconnect(btd700_driver_t* drv) {
    if (!drv) return BTD700_ERR_INVALID_ARG;
    const uint8_t arg = 0;
    return send_and_receive(drv, CMD_SET_BT_CONNECT, &arg, 1, NULL, NULL);
}

btd700_error_t btd700_driver_factory_reset(btd700_driver_t* drv) {
    if (!drv) return BTD700_ERR_INVALID_ARG;
    return send_and_receive(drv, CMD_SET_FACTORY_RESET, NULL, 0, NULL, NULL);
}

btd700_error_t btd700_driver_set_event_callback(btd700_driver_t* drv,
                                                 const btd700_event_callback_t callback,
                                                 void* user_data) {
    if (!drv) return BTD700_ERR_INVALID_ARG;
    drv->event_cb = callback;
    drv->event_user_data = user_data;
    return BTD700_OK;
}

btd700_error_t btd700_driver_poll_events(btd700_driver_t* drv, const int timeout_ms) {
    if (!drv) return BTD700_ERR_INVALID_ARG;
    if (!drv->handle) return BTD700_ERR_DEVICE_NOT_OPEN;

    uint8_t buf[64];

    const int res = hid_read_timeout(drv->handle, buf, sizeof(buf), timeout_ms);
    if (res < 0) return BTD700_ERR_HID;

    if (res > 0 && buf[0] == BTD700_REPORT_ID && buf[1] == BTD700_MARKER_DONGLE_CMD) {
        dispatch_event(drv, buf, res);
    }

    return BTD700_OK;
}

btd700_error_t btd700_driver_send_command(btd700_driver_t* drv,
                                           const uint8_t cmd,
                                           const uint8_t* args, const size_t args_len,
                                           uint8_t* out_buf, const size_t out_buf_size,
                                           size_t* out_len) {
    if (!drv) return BTD700_ERR_INVALID_ARG;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    const btd700_error_t err = send_and_receive(drv, cmd, args, args_len, resp, &resp_len);
    if (err != BTD700_OK) return err;

    if (out_len) *out_len = resp_len;

    if (out_buf && out_buf_size > 0) {
        const size_t copy_len = resp_len < out_buf_size ? resp_len : out_buf_size;
        memcpy(out_buf, resp, copy_len);
    }

    return BTD700_OK;
}

// ==========================================================================================
// string helpers
// ==========================================================================================

const char* btd700_audio_mode_string(const btd700_audio_mode_t mode) {
    switch (mode) {
        case BTD700_AUDIO_MODE_HIGH_QUALITY: return "High Quality";
        case BTD700_AUDIO_MODE_GAMING:       return "Gaming";
        case BTD700_AUDIO_MODE_BROADCAST:    return "Broadcast";
    }
    return "Unknown";
}

const char* btd700_transport_mode_string(const btd700_transport_mode_t mode) {
    switch (mode) {
        case BTD700_TRANSPORT_DISCONNECTED: return "Disconnected";
        case BTD700_TRANSPORT_CLASSIC:      return "Classic";
        case BTD700_TRANSPORT_LE_AUDIO:     return "LE Audio";
        case BTD700_TRANSPORT_MULTIPOINT:   return "Multipoint";
    }
    return "Unknown";
}

const char* btd700_dongle_state_string(const btd700_dongle_state_t state) {
    switch (state) {
        case BTD700_STATE_NONE:            return "None";
        case BTD700_STATE_DISCONNECTED:    return "Disconnected";
        case BTD700_STATE_CONNECTED:       return "Connected";
        case BTD700_STATE_STREAMING_AUDIO: return "Streaming Audio";
        case BTD700_STATE_STREAMING_VOICE: return "Streaming Voice";
    }
    return "Unknown";
}

const char* btd700_le_audio_state_string(const btd700_le_audio_state_t state) {
    switch (state) {
        case BTD700_LE_AUDIO_NONE:                return "None";
        case BTD700_LE_AUDIO_DISCONNECTED:        return "Disconnected";
        case BTD700_LE_AUDIO_CONNECTED:           return "Connected";
        case BTD700_LE_AUDIO_STREAMING_UNICAST:   return "Streaming Unicast";
        case BTD700_LE_AUDIO_STREAMING_BROADCAST: return "Streaming Broadcast";
    }
    return "Unknown";
}

const char* btd700_sink_mode_string(const btd700_sink_mode_t mode) {
    switch (mode) {
        case BTD700_SINK_NOT_AVAILABLE: return "Not Available";
        case BTD700_SINK_CLASSIC:       return "Classic";
        case BTD700_SINK_LE_AUDIO:      return "LE Audio";
        case BTD700_SINK_DUAL:          return "Dual";
    }
    return "Unknown";
}

const char* btd700_codec_string(const btd700_codec_t codec) {
    switch (codec) {
        case BTD700_CODEC_SBC:           return "SBC";
        case BTD700_CODEC_APTX:          return "aptX";
        case BTD700_CODEC_APTX_ADAPTIVE: return "aptX Adaptive";
        case BTD700_CODEC_APTX_LOSSLESS: return "aptX Lossless";
        case BTD700_CODEC_APTX_LITE:     return "aptX Lite";
        case BTD700_CODEC_LC3:           return "LC3";
    }
    return "Unknown";
}

const char* btd700_audio_frequency_string(const btd700_audio_frequency_t freq) {
    switch (freq) {
        case BTD700_FREQ_44100: return "44.1 kHz";
        case BTD700_FREQ_48000: return "48 kHz";
        case BTD700_FREQ_96000: return "96 kHz";
    }
    return "Unknown";
}

const char* btd700_audio_resolution_string(const btd700_audio_resolution_t res) {
    switch (res) {
        case BTD700_RES_16BIT: return "16-bit";
        case BTD700_RES_24BIT: return "24-bit";
    }
    return "Unknown";
}

const char* btd700_broadcast_state_string(const btd700_broadcast_state_t state) {
    switch (state) {
        case BTD700_BROADCAST_OFF_PRIVATE: return "Private/Off";
        case BTD700_BROADCAST_ON_PUBLIC:   return "Public";
    }
    return "Unknown";
}

const char* btd700_broadcast_encryption_string(const btd700_broadcast_encryption_t enc) {
    switch (enc) {
        case BTD700_BROADCAST_ENCRYPTION_OFF: return "Disabled";
        case BTD700_BROADCAST_ENCRYPTION_ON:  return "Enabled";
    }
    return "Unknown";
}

const char* btd700_broadcast_quality_string(const btd700_broadcast_quality_t quality) {
    switch (quality) {
        case BTD700_BROADCAST_QUALITY_STANDARD_16K: return "Standard 16k";
        case BTD700_BROADCAST_QUALITY_STANDARD_24K: return "Standard 24k";
        case BTD700_BROADCAST_QUALITY_HIGH:         return "High Quality";
    }
    return "Unknown";
}

const char* btd700_error_string(const btd700_error_t err) {
    switch (err) {
        case BTD700_OK:                   return "success";
        case BTD700_ERR_DEVICE_NOT_FOUND: return "device not found";
        case BTD700_ERR_DEVICE_NOT_OPEN:  return "device not open";
        case BTD700_ERR_HID:              return "HID error";
        case BTD700_ERR_INVALID_ARG:      return "invalid argument";
    }
    return "unknown error";
}
