/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Paul Sokolovsky
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

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "extmod/modwebsocket.h"

#if MICROPY_PY_WEBSOCKET

enum { FRAME_HEADER, FRAME_OPT, PAYLOAD, CONTROL, DONE };

enum { BLOCKING_WRITE = 0x80 };
#define BLOCKING BLOCKING_WRITE

typedef struct _mp_obj_websocket_t {
    mp_obj_base_t base;
    mp_obj_t sock;
    uint32_t msg_sz;
    mp_int_t timeout;
    byte mask[4];
    byte state;
    byte to_recv;
    byte mask_pos;
    byte buf_pos;
    byte buf[6];
    byte opts;
    // Copy of last data frame flags
    byte ws_flags;
    // Copy of current frame flags
    byte last_flags;
    bool has_setblocking;
} mp_obj_websocket_t;

static mp_uint_t websocket_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode);
static mp_uint_t websocket_write_raw(mp_obj_t self_in, const byte *header, mp_uint_t hdr_sz, const byte *buf, mp_uint_t size, int *errcode);

static mp_obj_t websocket_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sock,     MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL } },
        { MP_QSTR_blocking, MP_ARG_BOOL,                  {.u_bool = false } },
        { MP_QSTR_timeout,  MP_ARG_INT | MP_ARG_KW_ONLY,  {.u_int = -1 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_get_stream_raise(args[0].u_obj, MP_STREAM_OP_READ | MP_STREAM_OP_WRITE | MP_STREAM_OP_IOCTL);
    mp_obj_websocket_t *o = mp_obj_malloc(mp_obj_websocket_t, type);
    o->sock = args[0].u_obj;
    o->state = FRAME_HEADER;
    o->to_recv = 2;
    o->mask_pos = 0;
    o->buf_pos = 0;
    o->opts = FRAME_TXT;
    o->timeout = args[2].u_int;
    if (args[1].u_bool) {
        o->opts |= BLOCKING;
    }
    mp_obj_t dest[2];
    mp_load_method_protected(o->sock, MP_QSTR_setblocking, dest, false);
    o->has_setblocking = dest[0] != MP_OBJ_NULL;

    return MP_OBJ_FROM_PTR(o);
}

static mp_uint_t websocket_read_nonblocking(mp_obj_websocket_t *self, const mp_stream_p_t *stream, byte *buffer, mp_uint_t length, mp_uint_t tick_start, int *errcode, bool timeout_enabled) {
    mp_uint_t total_read = 0;
    while (total_read < length) {
        mp_uint_t read = stream->read(self->sock, buffer + total_read, length - total_read, errcode);
        mp_uint_t tick_current = mp_hal_ticks_ms();
        if (timeout_enabled && ((tick_current - tick_start) > (mp_uint_t)self->timeout)) {
            // DEBUG_printf("Read operation timed out after %ld ms\n", tick_current - tick_start);
            *errcode = MP_ETIMEDOUT;
            return MP_STREAM_ERROR;
        }
        if (read == MP_STREAM_ERROR) {
            if (*errcode == MP_EAGAIN) {
                mp_hal_delay_ms(1);
                continue;
            }
            return MP_STREAM_ERROR;
        }
        total_read += read;
    }
    *errcode = 0;
    return total_read;
}

static mp_uint_t websocket_read_inner(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    mp_obj_websocket_t *self = MP_OBJ_TO_PTR(self_in);
    const mp_stream_p_t *stream_p = mp_get_stream(self->sock);
    self->state = FRAME_HEADER;
    mp_uint_t start_tick = mp_hal_ticks_ms();
    while (true) {
        if (self->to_recv != 0) {
            mp_uint_t out_sz = websocket_read_nonblocking(self, stream_p, self->buf + self->buf_pos, self->to_recv, start_tick, errcode, self->timeout >= 0);
            if (out_sz == MP_STREAM_ERROR || out_sz == 0) {
                return out_sz;
            }
            self->buf_pos += out_sz;
            self->to_recv -= out_sz;
            if (self->to_recv != 0) {
                *errcode = MP_EAGAIN;
                return MP_STREAM_ERROR;
            }
        }

        switch (self->state) {
            case FRAME_HEADER: {
                // TODO: Split frame handling below is untested so far, so conservatively disable it

                // "Control frames MAY be injected in the middle of a fragmented message."
                // So, they must be processed before data frames (and not alter
                // self->ws_flags)
                byte frame_type = self->buf[0];
                self->last_flags = frame_type;
                frame_type &= FRAME_OPCODE_MASK;

                if ((self->buf[0] & FRAME_OPCODE_MASK) == FRAME_CONT) {
                    // Preserve previous frame type
                    self->ws_flags = (self->ws_flags & FRAME_OPCODE_MASK) | (self->buf[0] & ~FRAME_OPCODE_MASK);
                } else {
                    self->ws_flags = self->buf[0];
                }

                // Reset mask in case someone will use "simplified" protocol
                // without masks.
                memset(self->mask, 0, sizeof(self->mask));

                int to_recv = 0;
                size_t sz = self->buf[1] & 0x7f;
                if (sz == 126) {
                    // Msg size is next 2 bytes
                    to_recv += 2;
                } else if (sz == 127) {
                    // Msg size is next 8 bytes
                    assert(0);
                }
                if (self->buf[1] & 0x80) {
                    // Next 4 bytes is mask
                    to_recv += 4;
                }

                self->buf_pos = 0;
                self->to_recv = to_recv;
                self->msg_sz = sz; // May be overridden by FRAME_OPT
                if (to_recv != 0) {
                    self->state = FRAME_OPT;
                } else {
                    if (frame_type >= FRAME_CLOSE) {
                        self->state = CONTROL;
                    } else {
                        self->state = PAYLOAD;
                    }
                }
                continue;
            }

            case FRAME_OPT: {
                if ((self->buf_pos & 3) == 2) {
                    // First two bytes are message length
                    self->msg_sz = (self->buf[0] << 8) | self->buf[1];
                }
                if (self->buf_pos >= 4) {
                    // Last 4 bytes is mask
                    memcpy(self->mask, self->buf + self->buf_pos - 4, 4);
                }
                self->buf_pos = 0;
                if ((self->last_flags & FRAME_OPCODE_MASK) >= FRAME_CLOSE) {
                    self->state = CONTROL;
                } else {
                    self->state = PAYLOAD;
                }
                continue;
            }

            case PAYLOAD:
            case CONTROL: {
                mp_uint_t out_sz = 0;
                if (self->msg_sz == 0) {
                    // In case message had zero payload
                    goto no_payload;
                }

                size_t sz = MIN(size, self->msg_sz);
                out_sz = websocket_read_nonblocking(self, stream_p, buf, sz, start_tick, errcode, self->timeout >= 0);
                if (out_sz == MP_STREAM_ERROR || out_sz == 0) {
                    return out_sz;
                }
                sz = out_sz;
                for (byte *p = buf; sz--; p++) {
                    *p ^= self->mask[self->mask_pos++ & 3];
                }

                self->msg_sz -= out_sz;
                if (self->msg_sz == 0) {
                    byte last_state;
                no_payload:
                    last_state = self->state;
                    self->state = FRAME_HEADER;
                    self->to_recv = 2;
                    self->mask_pos = 0;
                    self->buf_pos = 0;

                    // Handle control frame
                    if (last_state == CONTROL) {
                        byte frame_type = self->last_flags & FRAME_OPCODE_MASK;
                        if (frame_type == FRAME_CLOSE) {
                            static const byte close_resp[2] = {0x88, 0};
                            int err;
                            websocket_write_raw(self_in, close_resp, sizeof(close_resp), close_resp, 0, &err);
                            return 0;
                        }

                        // DEBUG_printf("Finished receiving ctrl message %x, ignoring\n", self->last_flags);
                        continue;
                    }
                }

                if (out_sz != 0) {
                    return out_sz;
                }
                // Empty (data) frame received is not EOF
                continue;
            }

        }
    }
}

static mp_uint_t websocket_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    mp_obj_websocket_t *self = MP_OBJ_TO_PTR(self_in);

    mp_obj_t dest[3];
    if (self->has_setblocking) {
        mp_load_method(self->sock, MP_QSTR_setblocking, dest);
        dest[2] = mp_const_false;
        mp_call_method_n_kw(1, 0, dest);
    }

    mp_uint_t result = websocket_read_inner(self_in, buf, size, errcode);

    if (self->has_setblocking) {
        dest[2] = (self->opts & BLOCKING) ? mp_const_true : mp_const_false;
        mp_call_method_n_kw(1, 0, dest);
    }

    return result;
}

static mp_uint_t websocket_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    mp_obj_websocket_t *self = MP_OBJ_TO_PTR(self_in);
    assert(size < 0x10000);
    byte header[4] = {0x80 | (self->opts & FRAME_OPCODE_MASK)};
    int hdr_sz;
    if (size < 126) {
        header[1] = size;
        hdr_sz = 2;
    } else {
        header[1] = 126;
        header[2] = size >> 8;
        header[3] = size & 0xff;
        hdr_sz = 4;
    }

    return websocket_write_raw(self_in, header, hdr_sz, buf, size, errcode);
}

static mp_uint_t websocket_write_raw(mp_obj_t self_in, const byte *header, mp_uint_t hdr_sz, const byte *buf, mp_uint_t size, int *errcode) {
    mp_obj_websocket_t *self = MP_OBJ_TO_PTR(self_in);
    const mp_stream_p_t *stream_p = mp_get_stream(self->sock);

    mp_obj_t dest[3];
    if (self->has_setblocking) {
        mp_load_method(self->sock, MP_QSTR_setblocking, dest);
        dest[2] = mp_const_false;
        mp_call_method_n_kw(1, 0, dest);
    }

    self->state = FRAME_HEADER;
    mp_uint_t out_sz = MP_STREAM_ERROR;
    mp_uint_t start_ticks = mp_hal_ticks_ms();
    mp_uint_t offset = 0;
    while (true) {
        switch (self->state) {
            case FRAME_HEADER: {
                out_sz = stream_p->write(self->sock, header + offset, hdr_sz - offset, errcode);
                if (out_sz == MP_STREAM_ERROR) {
                    if (*errcode != MP_EAGAIN) {
                        goto done;
                    }
                    *errcode = 0;
                    out_sz = 0;
                }
                offset += out_sz;
                if (offset >= hdr_sz) {
                    offset = 0;
                    self->state = PAYLOAD;
                }
                break;
            }

            case PAYLOAD: {
                out_sz = stream_p->write(self->sock, buf + offset, size - offset, errcode);
                if (out_sz == MP_STREAM_ERROR) {
                    if (*errcode != MP_EAGAIN) {
                        goto done;
                    }
                    *errcode = 0;
                    out_sz = 0;
                }
                offset += out_sz;
                if (offset >= size) {
                    self->state = DONE;
                }
                break;
            }

            case DONE: {
                out_sz = size;
                goto done;
            }

            default:
                assert(!"Shouldn't get here");
                MP_UNREACHABLE;
        }

        mp_uint_t current_ticks = mp_hal_ticks_ms();
        if ((current_ticks - start_ticks) > (mp_uint_t)self->timeout) {
            // DEBUG_printf("Write operation timed out after %ld ms\n", current_ticks - start_ticks);
            *errcode = MP_ETIMEDOUT;
            break;
        }
        mp_hal_delay_ms(1);
    }

done:
    if (self->has_setblocking) {
        dest[2] = (self->opts & BLOCKING) ? mp_const_true : mp_const_false;
        mp_call_method_n_kw(1, 0, dest);
    }

    return *errcode != 0 ? MP_STREAM_ERROR : out_sz;
}

static mp_uint_t websocket_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    mp_obj_websocket_t *self = MP_OBJ_TO_PTR(self_in);
    switch (request) {
        case MP_STREAM_CLOSE:
            // TODO: Send close signaling to the other side, otherwise it's
            // abrupt close (connection abort).
            mp_stream_close(self->sock);
            return 0;
        case MP_STREAM_GET_DATA_OPTS:
            return self->ws_flags & FRAME_OPCODE_MASK;
        case MP_STREAM_SET_DATA_OPTS: {
            int cur = self->opts & FRAME_OPCODE_MASK;
            self->opts = (self->opts & ~FRAME_OPCODE_MASK) | (arg & FRAME_OPCODE_MASK);
            return cur;
        }
        case MP_STREAM_TIMEOUT: {
            mp_int_t timeout = self->timeout;
            self->timeout = arg;
            return timeout;
        }
        default:
            *errcode = MP_EINVAL;
            return MP_STREAM_ERROR;
    }
}

static const mp_rom_map_elem_t websocket_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_ioctl), MP_ROM_PTR(&mp_stream_ioctl_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
};
static MP_DEFINE_CONST_DICT(websocket_locals_dict, websocket_locals_dict_table);

static const mp_stream_p_t websocket_stream_p = {
    .read = websocket_read,
    .write = websocket_write,
    .ioctl = websocket_ioctl,
};

static MP_DEFINE_CONST_OBJ_TYPE(
    websocket_type,
    MP_QSTR_websocket,
    MP_TYPE_FLAG_NONE,
    make_new, websocket_make_new,
    protocol, &websocket_stream_p,
    locals_dict, &websocket_locals_dict
    );

static const mp_rom_map_elem_t websocket_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_websocket) },
    { MP_ROM_QSTR(MP_QSTR_websocket), MP_ROM_PTR(&websocket_type) },
};

static MP_DEFINE_CONST_DICT(websocket_module_globals, websocket_module_globals_table);

const mp_obj_module_t mp_module_websocket = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&websocket_module_globals,
};

// This module should not be extensible (as it is not a CPython standard
// library nor is it necessary to override from the filesystem), however it
// has previously been known as `uwebsocket`, so by making it extensible the
// `uwebsocket` alias will continue to work.
MP_REGISTER_EXTENSIBLE_MODULE(MP_QSTR_websocket, mp_module_websocket);

#endif // MICROPY_PY_WEBSOCKET
