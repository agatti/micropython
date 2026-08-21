/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * Development of the code in this file was sponsored by Microbric Pty Ltd
 * and Mnemote Pty Ltd
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016, 2017 Nick Moore @mnemote
 * Copyright (c) 2017 "Eric Poulsen" <eric@zyxod.com>
 *
 * Based on esp8266/modnetwork.c which is Copyright (c) 2015 Paul Sokolovsky
 * And the ESP IDF example code which is Public Domain / CC0
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <string.h>

#include "py/objlist.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "extmod/modnetwork.h"
#include "shared/netutils/netutils.h"
#include "modnetwork.h"

#include "esp_wifi.h"

#if MICROPY_PY_NETWORK_WLAN_CSI
#include "network_wlan_csi.h"
#endif
#if MICROPY_PY_NETWORK_WLAN_DPP
#include "esp_dpp.h"
#endif
#include "esp_log.h"
#include "esp_psram.h"
#if !CONFIG_ESP_HOSTED_ENABLED
#include "esp_wifi_ap_get_sta_list.h"
#endif

#ifndef NO_QSTR
#include "mdns.h"
#endif

#if MICROPY_PY_NETWORK_WLAN

#if (WIFI_MODE_STA & WIFI_MODE_AP != WIFI_MODE_NULL || WIFI_MODE_STA | WIFI_MODE_AP != WIFI_MODE_APSTA)
#error WIFI_MODE_STA and WIFI_MODE_AP are supposed to be bitfields!
#endif

typedef base_if_obj_t wlan_if_obj_t;

static wlan_if_obj_t wlan_sta_obj;
static wlan_if_obj_t wlan_ap_obj;

// Set to "true" if esp_wifi_start() was called
static bool wifi_started = false;

// Set to "true" if the STA interface is requested to be connected by the
// user, used for automatic reassociation.
static bool wifi_sta_connect_requested = false;

// Set to "true" if the STA interface is connected to wifi and has IP address.
static bool wifi_sta_connected = false;

// Store the current status. 0 means None here, safe to do so as first enum value is WIFI_REASON_UNSPECIFIED=1.
static uint8_t wifi_sta_disconn_reason = 0;

#if MICROPY_HW_ENABLE_MDNS_QUERIES || MICROPY_HW_ENABLE_MDNS_RESPONDER
// Whether mDNS has been initialised or not (shared with network_lan.c)
bool mdns_initialised = false;
#endif

static uint8_t conf_wifi_sta_reconnects = 0;
static uint8_t wifi_sta_reconnects;

// The rules for this default are defined in the documentation of esp_wifi_set_protocol()
// rather than in code, so we have to recreate them here.
#if CONFIG_SOC_WIFI_HE_SUPPORT
// Note: No Explicit support for 5GHz here, yet
#define WIFI_PROTOCOL_DEFAULT (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX)
#else
#define WIFI_PROTOCOL_DEFAULT (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)
#endif

#if MICROPY_PY_NETWORK_WLAN_DPP

enum {
    DPP_STATE_WAIT,
    DPP_STATE_READY,
    DPP_STATE_ERROR,
};

static struct wifi_dpp_state_t {
    // This must be allocated and freed via heap_caps_malloc/heap_caps_free.
    char *qr_code_buffer;
    ssize_t qr_code_length; // -1 indicates failure
    wifi_config_t configuration;
    volatile bool listening;
    volatile int state;
    volatile int error;
} wifi_dpp_state;

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0))
#define EVT_DPP_URI_READY (WIFI_EVENT_DPP_URI_READY)
#define EVT_DPP_CFG_RECVD (WIFI_EVENT_DPP_CFG_RECVD)
#define EVT_DPP_FAILED (WIFI_EVENT_DPP_FAILED)
#else
#define EVT_DPP_URI_READY (ESP_SUPP_DPP_URI_READY)
#define EVT_DPP_CFG_RECVD (ESP_SUPP_DPP_CFG_RECVD)
#define EVT_DPP_FAILED (ESP_SUPP_DPP_FAIL)
#endif

// Merge this with the main WiFi event handler once ESP-IDF 5.5.x is the minimum
// supported version.
static void esp_wifi_dpp_callback(esp_supp_dpp_event_t evt, void *data) {
    // URI_READY event is sent during bootstrap and within the scope of
    // WLAN.dpp_start, so the callback shouldn't be able to receive stray
    // URI_READY events.
    if ((int)evt != EVT_DPP_URI_READY && !wifi_dpp_state.listening) {
        return;
    }

    switch ((int)evt) {
        case EVT_DPP_URI_READY: {
            assert(!wifi_dpp_state.qr_code_buffer && wifi_dpp_state.qr_code_length == 0 && "QR code variables went out of sync");
            const wifi_event_dpp_uri_ready_t *event_data = (const wifi_event_dpp_uri_ready_t *)data;
            char *new_buffer = heap_caps_malloc(event_data->uri_data_len, MALLOC_CAP_DEFAULT);
            if (!new_buffer) {
                wifi_dpp_state.qr_code_length = -1;
                return;
            }
            wifi_dpp_state.qr_code_buffer = new_buffer;
            memcpy(wifi_dpp_state.qr_code_buffer, event_data->uri, event_data->uri_data_len);
            wifi_dpp_state.qr_code_length = (ssize_t)event_data->uri_data_len;
            assert(wifi_dpp_state.state == DPP_STATE_WAIT && "DPP state out of sync");
            break;
        }
        case EVT_DPP_CFG_RECVD: {
            // Provisioning data obtained.
            memcpy(&wifi_dpp_state.configuration, &((const wifi_event_dpp_config_received_t *)data)->wifi_cfg, sizeof(wifi_config_t));
            esp_err_t result = esp_supp_dpp_stop_listen();
            if (result != ESP_OK) {
                wifi_dpp_state.state = DPP_STATE_ERROR;
                wifi_dpp_state.error = result;
            } else {
                wifi_dpp_state.listening = false;
                result = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_dpp_state.configuration);
                if (result != ESP_OK) {
                    wifi_dpp_state.state = DPP_STATE_ERROR;
                    wifi_dpp_state.error = result;
                }
            }
            wifi_dpp_state.state = DPP_STATE_READY;
            break;
        }
        case EVT_DPP_FAILED:
            const wifi_event_dpp_failed_t *event_data = (const wifi_event_dpp_failed_t *)data;
            wifi_dpp_state.state = DPP_STATE_ERROR;
            wifi_dpp_state.error = event_data->failure_reason;
            break;

        default:
            return;
    }
}
#endif

// This function is called by the system-event task and so runs in a different
// thread to the main MicroPython task.  It must not raise any Python exceptions
// or allocate any memory in MicroPython's own heap.
static void network_wlan_wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI("wifi", "STA_START");
            wlan_sta_obj.active = true;
            wifi_sta_reconnects = 0;
            break;

        case WIFI_EVENT_STA_STOP:
            wlan_sta_obj.active = false;
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI("network", "CONNECTED");
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            // This is a workaround as ESP32 WiFi libs don't currently
            // auto-reassociate.

            wifi_event_sta_disconnected_t *disconn = event_data;
            char *message = "";
            wifi_sta_disconn_reason = disconn->reason;
            switch (disconn->reason) {
                case WIFI_REASON_BEACON_TIMEOUT:
                    // AP has dropped out; try to reconnect.
                    message = "beacon timeout";
                    break;
                case WIFI_REASON_NO_AP_FOUND:
                    // AP may not exist, or it may have momentarily dropped out; try to reconnect.
                    message = "no AP found";
                    break;
                case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
                    // No AP with RSSI within given threshold exists, or it may have momentarily dropped out; try to reconnect.
                    message = "no AP with RSSI within threshold found";
                    break;
                case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
                    // No AP with authmode within given threshold exists, or it may have momentarily dropped out; try to reconnect.
                    message = "no AP with authmode within threshold found";
                    break;
                case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
                    // No AP with compatible security exists, or it may have momentarily dropped out; try to reconnect.
                    message = "no AP with compatible security found";
                    break;
                case WIFI_REASON_AUTH_FAIL:
                    // Password may be wrong, or it just failed to connect; try to reconnect.
                    message = "authentication failed";
                    break;
                default:
                    // Let other errors through and try to reconnect.
                    break;
            }
            ESP_LOGI("wifi", "STA_DISCONNECTED, reason:%d:%s", disconn->reason, message);

            wifi_sta_connected = false;
            if (wifi_sta_connect_requested) {
                wifi_mode_t mode;
                if (esp_wifi_get_mode(&mode) != ESP_OK) {
                    break;
                }
                if (!(mode & WIFI_MODE_STA)) {
                    break;
                }
                if (conf_wifi_sta_reconnects) {
                    ESP_LOGI("wifi", "reconnect counter=%d, max=%d",
                        wifi_sta_reconnects, conf_wifi_sta_reconnects);
                    if (++wifi_sta_reconnects >= conf_wifi_sta_reconnects) {
                        break;
                    }
                }
                esp_err_t e = esp_wifi_connect();
                if (e != ESP_OK) {
                    ESP_LOGI("wifi", "error attempting to reconnect: 0x%04x", e);
                }
            }
            break;
        }

        case WIFI_EVENT_AP_START:
            wlan_ap_obj.active = true;
            break;

        case WIFI_EVENT_AP_STOP:
            wlan_ap_obj.active = false;
            break;

        #if MICROPY_PY_NETWORK_WLAN_DPP && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0))
        case WIFI_EVENT_DPP_URI_READY:
        case WIFI_EVENT_DPP_CFG_RECVD:
        case WIFI_EVENT_DPP_FAILED:
            esp_wifi_dpp_callback(event_id, event_data);
            break;
        #endif

        default:
            break;
    }
}

static void network_wlan_ip_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    switch (event_id) {
        case IP_EVENT_STA_GOT_IP:
            ESP_LOGI("network", "GOT_IP");
            wifi_sta_connected = true;
            wifi_sta_disconn_reason = 0; // Success so clear error. (in case of new error will be replaced anyway)
            #if MICROPY_HW_ENABLE_MDNS_QUERIES || MICROPY_HW_ENABLE_MDNS_RESPONDER
            if (!mdns_initialised) {
                mdns_init();
                #if MICROPY_HW_ENABLE_MDNS_RESPONDER
                mdns_hostname_set(mod_network_hostname_data);
                mdns_instance_name_set(mod_network_hostname_data);
                #endif
                mdns_initialised = true;
            }
            #endif
            break;

        default:
            break;
    }
}

static void require_if(mp_obj_t wlan_if, int if_no) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(wlan_if);
    if (self->if_id != if_no) {
        mp_raise_msg(&mp_type_OSError, if_no == ESP_IF_WIFI_STA ? MP_ERROR_TEXT("STA required") : MP_ERROR_TEXT("AP required"));
    }
}

void esp_initialise_wifi(void) {
    static int wifi_initialized = 0;
    if (!wifi_initialized) {
        esp_exceptions(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_wlan_wifi_event_handler, NULL, NULL));
        esp_exceptions(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, network_wlan_ip_event_handler, NULL, NULL));

        wlan_sta_obj.base.type = &esp_network_wlan_type;
        wlan_sta_obj.if_id = ESP_IF_WIFI_STA;
        wlan_sta_obj.netif = esp_netif_create_default_wifi_sta();
        wlan_sta_obj.active = false;

        wlan_ap_obj.base.type = &esp_network_wlan_type;
        wlan_ap_obj.if_id = ESP_IF_WIFI_AP;
        wlan_ap_obj.netif = esp_netif_create_default_wifi_ap();
        wlan_ap_obj.active = false;

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        #if CONFIG_SPIRAM_IGNORE_NOTFOUND
        if (!esp_psram_is_initialized()) {
            // If PSRAM failed to initialize, disable "Wi-Fi Cache TX Buffers"
            // (default SPIRAM config ESP32_WIFI_CACHE_TX_BUFFER_NUM==32, this is 54,400 bytes of heap)
            cfg.cache_tx_buf_num = 0;
            cfg.feature_caps &= ~CONFIG_FEATURE_CACHE_TX_BUF_BIT;

            // Set some other options back to the non-SPIRAM default values
            // to save more RAM.
            //
            // These can be determined from ESP-IDF components/esp_wifi/Kconfig and the
            // WIFI_INIT_CONFIG_DEFAULT macro
            cfg.tx_buf_type = 1; // Dynamic, this "magic number" is defined in IDF KConfig
            cfg.static_tx_buf_num = 0; // Probably don't need, due to tx_buf_type
            cfg.dynamic_tx_buf_num = 32; // ESP-IDF default value (maximum)
        }
        #endif
        ESP_LOGD("modnetwork", "Initializing WiFi");
        esp_exceptions(esp_wifi_init(&cfg));
        esp_exceptions(esp_wifi_set_storage(WIFI_STORAGE_RAM));

        ESP_LOGD("modnetwork", "Initialized");
        wifi_initialized = 1;
    }
}

static mp_obj_t network_wlan_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, false);

    esp_initialise_wifi();

    int idx = (n_args > 0) ? mp_obj_get_int(args[0]) : ESP_IF_WIFI_STA;
    if (idx == ESP_IF_WIFI_STA) {
        return MP_OBJ_FROM_PTR(&wlan_sta_obj);
    } else if (idx == ESP_IF_WIFI_AP) {
        return MP_OBJ_FROM_PTR(&wlan_ap_obj);
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid WLAN interface identifier"));
    }
}

static mp_obj_t network_wlan_active(size_t n_args, const mp_obj_t *args) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    wifi_mode_t mode;
    if (!wifi_started) {
        mode = WIFI_MODE_NULL;
    } else {
        esp_exceptions(esp_wifi_get_mode(&mode));
    }

    int bit = (self->if_id == ESP_IF_WIFI_STA) ? WIFI_MODE_STA : WIFI_MODE_AP;

    if (n_args > 1) {
        bool active = mp_obj_is_true(args[1]);
        mode = active ? (mode | bit) : (mode & ~bit);
        if (mode == WIFI_MODE_NULL) {
            if (wifi_started) {
                esp_exceptions(esp_wifi_stop());
                wifi_started = false;
            }
        } else {
            esp_exceptions(esp_wifi_set_mode(mode));
            if (!wifi_started) {
                esp_exceptions(esp_wifi_start());
                wifi_started = true;
            }
        }

        // Wait for the interface to be in the correct state.
        while (self->active != active) {
            MICROPY_EVENT_POLL_HOOK;
        }

        #if MICROPY_PY_NETWORK_WLAN_DPP
        if (self->if_id == ESP_IF_WIFI_STA && wifi_dpp_state.listening) {
            esp_exceptions(esp_supp_dpp_stop_listen());
            wifi_dpp_state.listening = false;
            wifi_dpp_state.state = DPP_STATE_WAIT;
            wifi_dpp_state.error = 0;
        }
        #endif
    }

    return (mode & bit) ? mp_const_true : mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_wlan_active_obj, 1, 2, network_wlan_active);

#if MICROPY_PY_NETWORK_WLAN_DPP
static mp_obj_t network_wlan_dpp_start(size_t n_args, const mp_obj_t *args) {
    require_if(args[0], ESP_IF_WIFI_STA);

    static int dpp_initialised = 0;
    if (!dpp_initialised) {
        #if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0))
        esp_exceptions(esp_supp_dpp_init(NULL));
        #else
        esp_exceptions(esp_supp_dpp_init(esp_wifi_dpp_callback));
        #endif
        dpp_initialised = 1;
    }

    // check enrollment method, only QR code is supported as of ESP-IDF 5.5.
    mp_int_t enrollment_method = mp_obj_get_int(args[2]);
    if (enrollment_method != DPP_BOOTSTRAP_QR_CODE) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid enrollment method"));
    }

    // build channels list
    size_t channels_count = 0;
    char *channels = NULL;
    if (mp_obj_is_tuple_compatible(args[1])) {
        mp_obj_t *channel_objects;
        mp_obj_tuple_get(args[1], &channels_count, &channel_objects);
        if (channels_count >= ESP_DPP_MAX_CHAN_COUNT) {
            mp_raise_ValueError(MP_ERROR_TEXT("too many channels"));
        }
        // This has to be encoded in ASCII...  To be forward-compatible with
        // 5GHz channel numbers, assume each channel can be up to 3 characters
        // long - then add separators and NULL terminator.
        channels = m_new0(char, channels_count * 4);
        char *current_pointer = channels;
        char channel_buffer[5] = {};
        for (size_t channel = 0; channel < channels_count; ++channel) {
            mp_int_t channel_id = mp_obj_get_int(channel_objects[channel]);
            if (!MP_FIT_UNSIGNED(8, channel_id)) {
                mp_raise_ValueError(MP_ERROR_TEXT("invalid channel"));
            }
            size_t len = sprintf(channel_buffer, "%d,", channel_id);
            memcpy(current_pointer, channel_buffer, len);
            current_pointer += len;
        }
        *(current_pointer - 1) = '\0';
    } else {
        mp_int_t channel = mp_obj_get_int(args[1]);
        if (!MP_FIT_UNSIGNED(8, channel)) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid channel"));
        }
        channels = m_new0(char, 4);
        sprintf(channels, "%d", channel);
    }

    // get key if present
    char *key = NULL;
    mp_buffer_info_t bufinfo;
    if (n_args > 3 && args[3] != mp_const_none) {
        mp_get_buffer_raise(args[3], &bufinfo, MP_BUFFER_READ);
        if (bufinfo.len != 32) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid buffer length"));
        }
        key = m_new0(char, 33);
        memcpy(key, bufinfo.buf, 32);
    }

    // get info if present
    char *info = NULL;
    if (n_args > 4 && args[4] != mp_const_none) {
        mp_get_buffer_raise(args[4], &bufinfo, MP_BUFFER_READ);
        info = m_new0(char, bufinfo.len + 1);
        memcpy(info, bufinfo.buf, bufinfo.len);
    }

    // Reset the URI buffer first.
    if (wifi_dpp_state.qr_code_buffer) {
        heap_caps_free(wifi_dpp_state.qr_code_buffer);
        wifi_dpp_state.qr_code_buffer = NULL;
        wifi_dpp_state.qr_code_length = 0;
    }
    wifi_dpp_state.state = DPP_STATE_WAIT;
    wifi_dpp_state.error = 0;

    esp_supp_dpp_stop_listen();  // Just in case.
    esp_exceptions(esp_supp_dpp_bootstrap_gen(channels, DPP_BOOTSTRAP_QR_CODE, key, info));

    // Loop until the URI is ready.  Since this is algorithmic and not depending
    // on external factors and expected to finish in reasonable time, looping on
    // the QR code length is somewhat acceptable.

    while (wifi_dpp_state.qr_code_length == 0) {
        MICROPY_EVENT_POLL_HOOK;
    }

    if (wifi_dpp_state.qr_code_length < 0) {
        // Out of memory whilst duplicating the QR code data.
        assert(wifi_dpp_state.qr_code_buffer == NULL && "Error reported with non-null buffer");
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("cannot allocate memory for QR code"));
    }

    mp_obj_t qr_code_string = mp_obj_new_str(wifi_dpp_state.qr_code_buffer, (size_t)wifi_dpp_state.qr_code_length);
    heap_caps_free(wifi_dpp_state.qr_code_buffer);
    wifi_dpp_state.qr_code_buffer = NULL;
    wifi_dpp_state.qr_code_length = 0;

    wifi_dpp_state.listening = true;
    esp_err_t error = esp_supp_dpp_start_listen();
    if (error != ESP_OK) {
        wifi_dpp_state.listening = false;
    }
    esp_exceptions(error);

    return qr_code_string;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_wlan_dpp_start_obj, 3, 5, network_wlan_dpp_start);

static mp_obj_t network_wlan_dpp_state(mp_obj_t self_in) {
    require_if(self_in, ESP_IF_WIFI_STA); // Can be removed if needed.

    // Rather than having a mutex just loop through, it shouldn't make more than
    // two iterations per value anyway.

    int state;
    int error;

    for (;;) {
        int state_check;
        do {
            state = wifi_dpp_state.state;
            state_check = wifi_dpp_state.state;
        } while (state != state_check);

        int error_check;
        do {
            error = wifi_dpp_state.error;
            error_check = wifi_dpp_state.error;
        } while (error != error_check);

        // Read between state and error update.
        if (state == DPP_STATE_ERROR && error == 0) {
            continue;
        }
        break;
    }

    mp_obj_t payload[2] = {
        MP_OBJ_NEW_SMALL_INT(state),
        MP_OBJ_NEW_SMALL_INT(error),
    };

    return MP_OBJ_FROM_PTR(mp_obj_new_tuple(2, payload));
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_wlan_dpp_state_obj, network_wlan_dpp_state);

static mp_obj_t network_wlan_dpp_stop(mp_obj_t self_in) {
    require_if(self_in, ESP_IF_WIFI_STA);

    if (wifi_dpp_state.listening) {
        esp_exceptions(esp_supp_dpp_stop_listen());
        wifi_dpp_state.listening = false;
        wifi_dpp_state.state = DPP_STATE_WAIT;
        wifi_dpp_state.error = 0;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_wlan_dpp_stop_obj, network_wlan_dpp_stop);
#endif

static mp_obj_t network_wlan_connect(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_ssid, ARG_key, ARG_bssid };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_, MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_, MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_bssid, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };

    // parse args
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    wifi_config_t wifi_sta_config = {0};

    // configure any parameters that are given
    if (n_args > 1) {
        size_t len;
        const char *p;
        if (args[ARG_ssid].u_obj != mp_const_none) {
            p = mp_obj_str_get_data(args[ARG_ssid].u_obj, &len);
            memcpy(wifi_sta_config.sta.ssid, p, MIN(len, sizeof(wifi_sta_config.sta.ssid)));
        }
        if (args[ARG_key].u_obj != mp_const_none) {
            p = mp_obj_str_get_data(args[ARG_key].u_obj, &len);
            memcpy(wifi_sta_config.sta.password, p, MIN(len, sizeof(wifi_sta_config.sta.password)));
        }
        if (args[ARG_bssid].u_obj != mp_const_none) {
            p = mp_obj_str_get_data(args[ARG_bssid].u_obj, &len);
            if (len != sizeof(wifi_sta_config.sta.bssid)) {
                mp_raise_ValueError(NULL);
            }
            wifi_sta_config.sta.bssid_set = 1;
            memcpy(wifi_sta_config.sta.bssid, p, sizeof(wifi_sta_config.sta.bssid));
        }

        #if MICROPY_PY_NETWORK_WLAN_DPP
        if (wifi_dpp_state.listening) {
            esp_exceptions(esp_supp_dpp_stop_listen());
            wifi_dpp_state.listening = false;
            wifi_dpp_state.state = DPP_STATE_WAIT;
            wifi_dpp_state.error = 0;
        }
        #endif

        esp_exceptions(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_sta_config));
    }

    esp_exceptions(esp_netif_set_hostname(wlan_sta_obj.netif, mod_network_hostname_data));

    wifi_sta_reconnects = 0;
    // connect to the WiFi AP
    MP_THREAD_GIL_EXIT();
    esp_exceptions(esp_wifi_connect());
    MP_THREAD_GIL_ENTER();
    wifi_sta_connect_requested = true;

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_wlan_connect_obj, 1, network_wlan_connect);

static mp_obj_t network_wlan_disconnect(mp_obj_t self_in) {
    wifi_sta_connect_requested = false;
    esp_exceptions(esp_wifi_disconnect());
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_wlan_disconnect_obj, network_wlan_disconnect);

static mp_obj_t network_wlan_status(size_t n_args, const mp_obj_t *args) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (n_args == 1) {
        if (self->if_id == ESP_IF_WIFI_STA) {
            // Case of no arg is only for the STA interface
            if (wifi_sta_connected) {
                // Happy path, connected with IP
                return MP_OBJ_NEW_SMALL_INT(STAT_GOT_IP);
            } else if (wifi_sta_disconn_reason == WIFI_REASON_NO_AP_FOUND) {
                return MP_OBJ_NEW_SMALL_INT(WIFI_REASON_NO_AP_FOUND);
            } else if (wifi_sta_disconn_reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD) {
                return MP_OBJ_NEW_SMALL_INT(WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD);
            } else if (wifi_sta_disconn_reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD) {
                return MP_OBJ_NEW_SMALL_INT(WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD);
            } else if (wifi_sta_disconn_reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY) {
                return MP_OBJ_NEW_SMALL_INT(WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY);
            } else if ((wifi_sta_disconn_reason == WIFI_REASON_AUTH_FAIL) || (wifi_sta_disconn_reason == WIFI_REASON_CONNECTION_FAIL)) {
                // wrong password
                return MP_OBJ_NEW_SMALL_INT(WIFI_REASON_AUTH_FAIL);
            } else if (wifi_sta_disconn_reason == WIFI_REASON_ASSOC_LEAVE) {
                // After wlan.disconnect()
                return MP_OBJ_NEW_SMALL_INT(STAT_IDLE);
            } else if (wifi_sta_connect_requested
                       && (conf_wifi_sta_reconnects == 0
                           || wifi_sta_reconnects < conf_wifi_sta_reconnects)) {
                // No connection or error, but is requested = Still connecting
                return MP_OBJ_NEW_SMALL_INT(STAT_CONNECTING);
            } else if (wifi_sta_disconn_reason == 0) {
                // No activity, No error = Idle
                return MP_OBJ_NEW_SMALL_INT(STAT_IDLE);
            } else {
                // Simply pass the error through from ESP-identifier
                return MP_OBJ_NEW_SMALL_INT(wifi_sta_disconn_reason);
            }
        }
        return mp_const_none;
    }

    // one argument: return status based on query parameter
    switch ((uintptr_t)args[1]) {
        case (uintptr_t)MP_OBJ_NEW_QSTR(MP_QSTR_stations): {
            // return list of connected stations, only if in soft-AP mode
            require_if(args[0], ESP_IF_WIFI_AP);
            wifi_sta_list_t station_list;
            esp_exceptions(esp_wifi_ap_get_sta_list(&station_list));
            #if !CONFIG_ESP_HOSTED_ENABLED
            wifi_sta_mac_ip_list_t mac_ip_list;
            esp_exceptions(esp_wifi_ap_get_sta_list_with_ip(&station_list, &mac_ip_list));
            #endif
            mp_obj_t list = mp_obj_new_list(0, NULL);
            #if CONFIG_ESP_HOSTED_ENABLED
            int count = station_list.num;
            wifi_sta_info_t *source = (wifi_sta_info_t *)station_list.sta;
            #else
            int count = mac_ip_list.num;
            esp_netif_pair_mac_ip_t *source = (esp_netif_pair_mac_ip_t *)mac_ip_list.sta;
            #endif
            for (int i = 0; i < count; ++i) {
                #if CONFIG_ESP_HOSTED_ENABLED
                mp_obj_tuple_t *t = mp_obj_new_tuple(1, NULL);
                #else
                mp_obj_tuple_t *t = mp_obj_new_tuple(2, NULL);
                #endif
                t->items[0] = mp_obj_new_bytes(source[i].mac, sizeof(source[i].mac));
                #if !CONFIG_ESP_HOSTED_ENABLED
                t->items[1] = source[i].ip.addr != 0 ? netutils_format_ipv4_addr((uint8_t *)(&source[i].ip), NETUTILS_BIG) : mp_const_none;
                #endif
                mp_obj_list_append(list, t);
            }
            return list;
        }
        case (uintptr_t)MP_OBJ_NEW_QSTR(MP_QSTR_rssi): {
            // return signal of AP, only in STA mode
            require_if(args[0], ESP_IF_WIFI_STA);

            wifi_ap_record_t info;
            esp_exceptions(esp_wifi_sta_get_ap_info(&info));
            return MP_OBJ_NEW_SMALL_INT(info.rssi);
        }
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("unknown status param"));
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_wlan_status_obj, 1, 2, network_wlan_status);

static mp_obj_t network_wlan_scan(mp_obj_t self_in) {
    // check that STA mode is active
    wifi_mode_t mode;
    esp_exceptions(esp_wifi_get_mode(&mode));
    if ((mode & WIFI_MODE_STA) == 0) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("STA must be active"));
    }

    mp_obj_t list = mp_obj_new_list(0, NULL);
    wifi_scan_config_t config = { 0 };
    config.show_hidden = true;
    MP_THREAD_GIL_EXIT();
    esp_err_t status = esp_wifi_scan_start(&config, 1);
    MP_THREAD_GIL_ENTER();
    if (status == 0) {
        uint16_t count = 0;
        esp_exceptions(esp_wifi_scan_get_ap_num(&count));
        if (count == 0) {
            // esp_wifi_scan_get_ap_records must be called to free internal buffers from the scan.
            // But it returns an error if wifi_ap_records==NULL.  So allocate at least 1 AP entry.
            // esp_wifi_scan_get_ap_records will then return the actual number of APs in count.
            count = 1;
        }
        wifi_ap_record_t *wifi_ap_records = calloc(count, sizeof(wifi_ap_record_t));
        esp_exceptions(esp_wifi_scan_get_ap_records(&count, wifi_ap_records));
        for (uint16_t i = 0; i < count; i++) {
            mp_obj_tuple_t *t = mp_obj_new_tuple(6, NULL);
            uint8_t *x = memchr(wifi_ap_records[i].ssid, 0, sizeof(wifi_ap_records[i].ssid));
            int ssid_len = x ? x - wifi_ap_records[i].ssid : sizeof(wifi_ap_records[i].ssid);
            t->items[0] = mp_obj_new_bytes(wifi_ap_records[i].ssid, ssid_len);
            t->items[1] = mp_obj_new_bytes(wifi_ap_records[i].bssid, sizeof(wifi_ap_records[i].bssid));
            t->items[2] = MP_OBJ_NEW_SMALL_INT(wifi_ap_records[i].primary);
            t->items[3] = MP_OBJ_NEW_SMALL_INT(wifi_ap_records[i].rssi);
            t->items[4] = MP_OBJ_NEW_SMALL_INT(wifi_ap_records[i].authmode);
            t->items[5] = mp_const_false; // XXX hidden?
            mp_obj_list_append(list, MP_OBJ_FROM_PTR(t));
        }
        free(wifi_ap_records);
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_wlan_scan_obj, network_wlan_scan);

static mp_obj_t network_wlan_isconnected(mp_obj_t self_in) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->if_id == ESP_IF_WIFI_STA) {
        return mp_obj_new_bool(wifi_sta_connected);
    } else {
        wifi_sta_list_t sta;
        esp_wifi_ap_get_sta_list(&sta);
        return mp_obj_new_bool(sta.num != 0);
    }
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_wlan_isconnected_obj, network_wlan_isconnected);

static mp_obj_t network_wlan_config(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    if (n_args != 1 && kwargs->used != 0) {
        mp_raise_TypeError(MP_ERROR_TEXT("either pos or kw args are allowed"));
    }

    wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    bool is_wifi = self->if_id == ESP_IF_WIFI_AP || self->if_id == ESP_IF_WIFI_STA;

    wifi_config_t cfg;
    if (is_wifi) {
        esp_exceptions(esp_wifi_get_config(self->if_id, &cfg));
    }

    if (kwargs->used != 0) {
        if (!is_wifi) {
            goto unknown;
        }

        for (size_t i = 0; i < kwargs->alloc; i++) {
            if (mp_map_slot_is_filled(kwargs, i)) {
                int req_if = -1;

                switch (mp_obj_str_get_qstr(kwargs->table[i].key)) {
                    case MP_QSTR_mac: {
                        mp_buffer_info_t bufinfo;
                        mp_get_buffer_raise(kwargs->table[i].value, &bufinfo, MP_BUFFER_READ);
                        if (bufinfo.len != 6) {
                            mp_raise_ValueError(MP_ERROR_TEXT("invalid buffer length"));
                        }
                        esp_exceptions(esp_wifi_set_mac(self->if_id, bufinfo.buf));
                        break;
                    }
                    case MP_QSTR_ssid:
                    case MP_QSTR_essid: {
                        req_if = ESP_IF_WIFI_AP;
                        size_t len;
                        const char *s = mp_obj_str_get_data(kwargs->table[i].value, &len);
                        len = MIN(len, sizeof(cfg.ap.ssid));
                        memcpy(cfg.ap.ssid, s, len);
                        cfg.ap.ssid_len = len;
                        break;
                    }
                    case MP_QSTR_hidden: {
                        req_if = ESP_IF_WIFI_AP;
                        cfg.ap.ssid_hidden = mp_obj_is_true(kwargs->table[i].value);
                        break;
                    }
                    case MP_QSTR_security:
                    case MP_QSTR_authmode: {
                        req_if = ESP_IF_WIFI_AP;
                        cfg.ap.authmode = mp_obj_get_int(kwargs->table[i].value);
                        break;
                    }
                    case MP_QSTR_key:
                    case MP_QSTR_password: {
                        req_if = ESP_IF_WIFI_AP;
                        size_t len;
                        const char *s = mp_obj_str_get_data(kwargs->table[i].value, &len);
                        len = MIN(len, sizeof(cfg.ap.password) - 1);
                        memcpy(cfg.ap.password, s, len);
                        cfg.ap.password[len] = 0;
                        break;
                    }
                    case MP_QSTR_channel: {
                        uint8_t channel = mp_obj_get_int(kwargs->table[i].value);
                        if (self->if_id == ESP_IF_WIFI_AP) {
                            cfg.ap.channel = channel;
                        } else {
                            // This setting is only used to determine the
                            // starting channel for a scan, so it can result in
                            // slightly faster connection times.
                            cfg.sta.channel = channel;

                            // This additional code to directly set the channel
                            // on the STA interface is only relevant for ESP-NOW
                            // (when there is no STA connection attempt.)
                            uint8_t old_primary;
                            wifi_second_chan_t secondary;
                            // Get the current value of secondary
                            esp_exceptions(esp_wifi_get_channel(&old_primary, &secondary));
                            esp_err_t err = esp_wifi_set_channel(channel, secondary);
                            if (err == ESP_ERR_INVALID_ARG) {
                                // May need to swap secondary channel above to below or below to above
                                secondary = (
                                    (secondary == WIFI_SECOND_CHAN_ABOVE)
                                    ? WIFI_SECOND_CHAN_BELOW
                                    : (secondary == WIFI_SECOND_CHAN_BELOW)
                                    ? WIFI_SECOND_CHAN_ABOVE
                                    : WIFI_SECOND_CHAN_NONE);
                                err = esp_wifi_set_channel(channel, secondary);
                            }
                            esp_exceptions(err);
                            if (channel != old_primary) {
                                // Workaround the ESP-IDF Wi-Fi stack sometimes taking a moment to change channels
                                mp_hal_delay_ms(1);
                            }
                        }
                        break;
                    }
                    case MP_QSTR_bandwidth: {
                        esp_exceptions(esp_wifi_set_bandwidth(self->if_id, mp_obj_get_int(kwargs->table[i].value)));
                        break;
                    }
                    case MP_QSTR_hostname:
                    case MP_QSTR_dhcp_hostname: {
                        // TODO: Deprecated. Use network.hostname(name) instead.
                        mod_network_hostname(1, &kwargs->table[i].value);
                        break;
                    }
                    case MP_QSTR_max_clients: {
                        req_if = ESP_IF_WIFI_AP;
                        cfg.ap.max_connection = mp_obj_get_int(kwargs->table[i].value);
                        break;
                    }
                    case MP_QSTR_reconnects: {
                        int reconnects = mp_obj_get_int(kwargs->table[i].value);
                        req_if = ESP_IF_WIFI_STA;
                        // parameter reconnects == -1 means to retry forever.
                        // here means conf_wifi_sta_reconnects == 0 to retry forever.
                        conf_wifi_sta_reconnects = (reconnects == -1) ? 0 : reconnects + 1;
                        break;
                    }
                    case MP_QSTR_txpower: {
                        int8_t power = (mp_obj_get_float(kwargs->table[i].value) * 4);
                        esp_exceptions(esp_wifi_set_max_tx_power(power));
                        break;
                    }
                    case MP_QSTR_protocol: {
                        esp_exceptions(esp_wifi_set_protocol(self->if_id, mp_obj_get_int(kwargs->table[i].value)));
                        break;
                    }
                    case MP_QSTR_pm: {
                        esp_exceptions(esp_wifi_set_ps(mp_obj_get_int(kwargs->table[i].value)));
                        break;
                    }
                    default:
                        goto unknown;
                }

                // We post-check interface requirements to save on code size
                if (req_if >= 0) {
                    require_if(args[0], req_if);
                }
            }
        }

        esp_exceptions(esp_wifi_set_config(self->if_id, &cfg));

        return mp_const_none;
    }

    // Get config

    if (n_args != 2) {
        mp_raise_TypeError(MP_ERROR_TEXT("can query only one param"));
    }

    int req_if = -1;
    mp_obj_t val = mp_const_none;

    switch (mp_obj_str_get_qstr(args[1])) {
        case MP_QSTR_mac: {
            uint8_t mac[6];
            switch (self->if_id) {
                case ESP_IF_WIFI_AP: // fallthrough intentional
                case ESP_IF_WIFI_STA:
                    esp_exceptions(esp_wifi_get_mac(self->if_id, mac));
                    return mp_obj_new_bytes(mac, sizeof(mac));
                default:
                    goto unknown;
            }
        }
        case MP_QSTR_ssid:
        case MP_QSTR_essid:
            switch (self->if_id) {
                case ESP_IF_WIFI_STA:
                    val = mp_obj_new_str_from_cstr((char *)cfg.sta.ssid);
                    break;
                case ESP_IF_WIFI_AP:
                    val = mp_obj_new_str((char *)cfg.ap.ssid, cfg.ap.ssid_len);
                    break;
                default:
                    req_if = ESP_IF_WIFI_AP;
            }
            break;
        case MP_QSTR_hidden:
            req_if = ESP_IF_WIFI_AP;
            val = mp_obj_new_bool(cfg.ap.ssid_hidden);
            break;
        case MP_QSTR_security:
        case MP_QSTR_authmode:
            req_if = ESP_IF_WIFI_AP;
            val = MP_OBJ_NEW_SMALL_INT(cfg.ap.authmode);
            break;
        case MP_QSTR_channel: {
            uint8_t channel;
            wifi_second_chan_t second;
            esp_exceptions(esp_wifi_get_channel(&channel, &second));
            val = MP_OBJ_NEW_SMALL_INT(channel);
            break;
        }
        case MP_QSTR_bandwidth: {
            wifi_bandwidth_t bandwidth;
            esp_exceptions(esp_wifi_get_bandwidth(self->if_id, &bandwidth));
            val = MP_OBJ_NEW_SMALL_INT(bandwidth);
            break;
        }
        case MP_QSTR_ifname: {
            val = esp_ifname(self->netif);
            break;
        }
        case MP_QSTR_hostname:
        case MP_QSTR_dhcp_hostname: {
            // TODO: Deprecated. Use network.hostname() instead.
            req_if = ESP_IF_WIFI_STA;
            val = mod_network_hostname(0, NULL);
            break;
        }
        case MP_QSTR_max_clients: {
            val = MP_OBJ_NEW_SMALL_INT(cfg.ap.max_connection);
            break;
        }
        case MP_QSTR_reconnects:
            req_if = ESP_IF_WIFI_STA;
            int rec = conf_wifi_sta_reconnects - 1;
            val = MP_OBJ_NEW_SMALL_INT(rec);
            break;
        case MP_QSTR_txpower: {
            int8_t power;
            esp_exceptions(esp_wifi_get_max_tx_power(&power));
            val = mp_obj_new_float(power * 0.25);
            break;
        }
        case MP_QSTR_protocol: {
            uint8_t protocol_bitmap;
            esp_exceptions(esp_wifi_get_protocol(self->if_id, &protocol_bitmap));
            val = MP_OBJ_NEW_SMALL_INT(protocol_bitmap);
            break;
        }
        case MP_QSTR_pm: {
            wifi_ps_type_t ps_type;
            esp_exceptions(esp_wifi_get_ps(&ps_type));
            val = MP_OBJ_NEW_SMALL_INT(ps_type);
            break;
        }
        default:
            goto unknown;
    }

    // We post-check interface requirements to save on code size
    if (req_if >= 0) {
        require_if(args[0], req_if);
    }

    return val;

unknown:
    mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
}
MP_DEFINE_CONST_FUN_OBJ_KW(network_wlan_config_obj, 1, network_wlan_config);

static const mp_rom_map_elem_t wlan_if_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&network_wlan_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&network_wlan_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&network_wlan_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&network_wlan_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_scan), MP_ROM_PTR(&network_wlan_scan_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&network_wlan_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_config), MP_ROM_PTR(&network_wlan_config_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig), MP_ROM_PTR(&esp_network_ifconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipconfig), MP_ROM_PTR(&esp_nic_ipconfig_obj) },
    #if MICROPY_PY_NETWORK_WLAN_DPP
    { MP_ROM_QSTR(MP_QSTR_dpp_start), MP_ROM_PTR(&network_wlan_dpp_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_dpp_state), MP_ROM_PTR(&network_wlan_dpp_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_dpp_stop), MP_ROM_PTR(&network_wlan_dpp_stop_obj) },
    #endif

    #if MICROPY_PY_NETWORK_WLAN_CSI
    { MP_ROM_QSTR(MP_QSTR_csi_enable), MP_ROM_PTR(&network_wlan_csi_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_csi_disable), MP_ROM_PTR(&network_wlan_csi_disable_obj) },
    { MP_ROM_QSTR(MP_QSTR_csi_read), MP_ROM_PTR(&network_wlan_csi_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_csi_dropped), MP_ROM_PTR(&network_wlan_csi_dropped_obj) },
    { MP_ROM_QSTR(MP_QSTR_csi_available), MP_ROM_PTR(&network_wlan_csi_available_obj) },
    #endif

    // Constants
    { MP_ROM_QSTR(MP_QSTR_IF_STA), MP_ROM_INT(WIFI_IF_STA)},
    { MP_ROM_QSTR(MP_QSTR_IF_AP), MP_ROM_INT(WIFI_IF_AP)},

    { MP_ROM_QSTR(MP_QSTR_SEC_OPEN), MP_ROM_INT(WIFI_AUTH_OPEN) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WEP), MP_ROM_INT(WIFI_AUTH_WEP) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA), MP_ROM_INT(WIFI_AUTH_WPA_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA2), MP_ROM_INT(WIFI_AUTH_WPA2_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA_WPA2), MP_ROM_INT(WIFI_AUTH_WPA_WPA2_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA2_ENT), MP_ROM_INT(WIFI_AUTH_WPA2_ENTERPRISE) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA3), MP_ROM_INT(WIFI_AUTH_WPA3_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA2_WPA3), MP_ROM_INT(WIFI_AUTH_WPA2_WPA3_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WAPI), MP_ROM_INT(WIFI_AUTH_WAPI_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_OWE), MP_ROM_INT(WIFI_AUTH_OWE) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA3_ENT_192), MP_ROM_INT(WIFI_AUTH_WPA3_ENT_192) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA3_EXT_PSK), MP_ROM_INT(WIFI_AUTH_WPA3_EXT_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA3_EXT_PSK_MIXED_MODE), MP_ROM_INT(WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE) },
    { MP_ROM_QSTR(MP_QSTR_SEC_DPP), MP_ROM_INT(WIFI_AUTH_DPP) },
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA3_ENT), MP_ROM_INT(WIFI_AUTH_WPA3_ENTERPRISE) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA2_WPA3_ENT), MP_ROM_INT(WIFI_AUTH_WPA2_WPA3_ENTERPRISE) },
    #endif
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 3)
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA_ENT), MP_ROM_INT(WIFI_AUTH_WPA_ENTERPRISE) },
    #endif

    { MP_ROM_QSTR(MP_QSTR_PROTOCOL_DEFAULT), MP_ROM_INT(WIFI_PROTOCOL_DEFAULT) },
    #if !CONFIG_IDF_TARGET_ESP32C2
    { MP_ROM_QSTR(MP_QSTR_PROTOCOL_LR), MP_ROM_INT(WIFI_PROTOCOL_LR) },
    #endif

    { MP_ROM_QSTR(MP_QSTR_PM_NONE), MP_ROM_INT(WIFI_PS_NONE) },
    { MP_ROM_QSTR(MP_QSTR_PM_PERFORMANCE), MP_ROM_INT(WIFI_PS_MIN_MODEM) },
    { MP_ROM_QSTR(MP_QSTR_PM_POWERSAVE), MP_ROM_INT(WIFI_PS_MAX_MODEM) },

    { MP_ROM_QSTR(MP_QSTR_BANDWIDTH_20), MP_ROM_INT(WIFI_BW20) },
    { MP_ROM_QSTR(MP_QSTR_BANDWIDTH_40), MP_ROM_INT(WIFI_BW40) },
    { MP_ROM_QSTR(MP_QSTR_BANDWIDTH_80), MP_ROM_INT(WIFI_BW80) },
    { MP_ROM_QSTR(MP_QSTR_BANDWIDTH_160), MP_ROM_INT(WIFI_BW160) },
    { MP_ROM_QSTR(MP_QSTR_BANDWIDTH_80_80), MP_ROM_INT(WIFI_BW80_BW80) },

    #if MICROPY_PY_NETWORK_WLAN_DPP
    { MP_ROM_QSTR(MP_QSTR_DPP_METHOD_QR), MP_ROM_INT(DPP_BOOTSTRAP_QR_CODE) },
    { MP_ROM_QSTR(MP_QSTR_DPP_STATE_WAIT), MP_ROM_INT(DPP_STATE_WAIT) },
    { MP_ROM_QSTR(MP_QSTR_DPP_STATE_READY), MP_ROM_INT(DPP_STATE_READY) },
    { MP_ROM_QSTR(MP_QSTR_DPP_STATE_ERROR), MP_ROM_INT(DPP_STATE_ERROR) },
    #endif
};
static MP_DEFINE_CONST_DICT(wlan_if_locals_dict, wlan_if_locals_dict_table);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 3)
_Static_assert(WIFI_AUTH_MAX == 17, "Synchronize WIFI_AUTH_XXX constants with the ESP-IDF. Look at esp-idf/components/esp_wifi/include/esp_wifi_types_generic.h");
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
_Static_assert(WIFI_AUTH_MAX == 16, "Synchronize WIFI_AUTH_XXX constants with the ESP-IDF. Look at esp-idf/components/esp_wifi/include/esp_wifi_types_generic.h");
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
_Static_assert(WIFI_AUTH_MAX == 14, "Synchronize WIFI_AUTH_XXX constants with the ESP-IDF. Look at esp-idf/components/esp_wifi/include/esp_wifi_types_generic.h");
#else
#error "Error in macro logic, all supported versions should be covered."
#endif

MP_DEFINE_CONST_OBJ_TYPE(
    esp_network_wlan_type,
    MP_QSTR_WLAN,
    MP_TYPE_FLAG_NONE,
    make_new, network_wlan_make_new,
    locals_dict, &wlan_if_locals_dict
    );

#endif // MICROPY_PY_NETWORK_WLAN
