/*
 * This file is part of the MicroPython project, https://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Alessandro Gatti
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

#include "py/mphal.h"

#if MICROPY_PY_GPIO

#ifndef __linux__
#error "Platform does not support GPIO access"
#endif

#if !MICROPY_PY_GPIO_STATIC_ALLOCATION
#error "Dynamic GPIO handling is not yet implemented"
#endif

#if MICROPY_PY_GPIO_IRQ && !MICROPY_PY_THREAD
#error "GPIO IRQ support needs threading to work"
#endif

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#if MICROPY_PY_GPIO_IRQ
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>
#endif

#include <linux/gpio.h>

#include "py/gc.h"
#include "py/misc.h"
#include "py/mperrno.h"
#include "py/obj.h"
#include "py/objint.h"
#include "py/objstr.h"
#include "py/qstr.h"
#include "py/runtime.h"

#include "pin.h"

#if MICROPY_DEBUG_VERBOSE
#define DEBUG_PRINT (1)
#define DEBUG_printf DEBUG_printf
#else
#define DEBUG_printf(...) (void)0
#endif

// Note: To keep the jargon equal across ports, Linux's GPIO devices will be
// called "ports" and GPIO lines will be called "pins" from here on.
//
// The GPIO management strategy used here is different than any other generic
// MicroPython port.
//
// General MCUs running MicroPython aren't really meant to run more than a
// single application and said application usually has exclusive and total
// control of any available I/O peripherals.
//
// With Linux, things are different:
//
// * GPIO pins' ownership by MicroPython could, in theory, not last for the
//   complete lifetime of the running code; pins can be claimed and released
//   at any time
//
// * GPIO pin access could be, if desired, non-exclusive; this means that if
//   a pin is claimed in non-exclusive mode its value must not be cached for
//   output pins because another process may have altered its state (or even
//   its mode!)
//
// * Claiming a GPIO pin may actually not succeed; if the pin was already
//   claimed in exclusive mode by another process then the claim will fail
//
// * GPIO ports may appear and disappear at any time; on embedded systems this
//   is usually not a concern as ports and pins are often allocated at kernel
//   boot time via device tree files; however there are USB devices that may
//   act as GPIO ports, and therefore no assumptions on ports and pins
//   availability must be made
//
// * Each pin can be tagged with a 32 bytes ASCII string (31 + terminator)
//   indicating the owner of the GPIO line; this is optional but not providing
//   it doesn't save any memory as each line structure has a fixed-size buffer
//   associated with it for this purpose - note that this value cannot be
//   changed unless the pin is released and claimed again
//
// * Linux GPIO support has no concept of alternate functions, as each I/O
//   operation targets a specialised device that will handle pin mapping behind
//   the scenes
//
// * If a GPIO port is removed from the system even though it is being used by
//   MicroPython (eg. removable device that gets yanked out of the machine),
//   things will *horribly* go out of sync as there's currently no provision
//   for listening to port events and invalidating all the pin instances
//   involved.
//
// This means the MicroPython GPIO Pin API needs to be tweaked to make sure
// these facts do not come in the way of regular usage:
//
// * The `Pin` class constructor gains three different keyword-only arguments:
//
//   * `port`, which is meant to receive either a number indicating which
//     /dev/gpiochipX character device to use for I/O requests, or a full
//     fledged filesystem path; if not provided it will default to
//     '/dev/gpiochip0'
//   * `consumer`, which is meant to receive a string that's up to 31 ASCII
//     bytes long containing the owner of the claimed GPIO pin; if not provided
//     it will default to "MicroPython"
//
// * A new instance method called `Pin.close` is introduced to manually give
//   control of the pin back to the operating system; this is the same as
//   manually calling `del` on the instance, but this allows for more
//   fine-grained control of the pin's ownership lifetime (this is safe to be
//   called multiple times, just like `del` is)
//
// * A new instance method called `Pin.available` is introduced to check
//   whether a pin instance can be used or if the underlying GPIO pin's
//   ownership has been already given back to the operating system
//
// * The `Pin` class is now context-aware, so `__enter__` and `__exit__`
//   instance methods have been added to make lifetime control easier to manage
//
// * Since GPIO ports are dynamic *no* `Pin` operation is guaranteed to
//   succeed, however on most if not all embedded systems GPIO pins are
//   provided by a device tree file and already set up and ready to go by the
//   time initrd has finished; however if the Pin instances are backed by an
//   external device that may get removed without notice, get ready for OSError
//   exceptions being raised on seemingly innocuous method calls
//
// * GPIO direction validity is strictly enforced: if a pin is defined as
//   input, setting its value will fail with an OSError; if you've got code
//   that pre-sets a GPIO value and toggles its direction later, it will work
//   on a microcontroller but will fail on Linux
//
// * For the time being, thread safety is *not guaranteed*, so use this
//   together with threads or asynchronous code at your own risk.
//
// And as usual: caveat emptor.

// TODO: Add `Pin.enumerate` to iterate through the entry names in
//       /sys/bus/gpio/devices and return the parsed output of calling
//       GPIO_GET_CHIPINFO_IOCTL on each device + GPIO_V2_GET_LINEINFO_IOCTL
//       on each line?

///////////////////////////////////////////////////////////////////////////////

typedef struct _gpio_port_t {
    mp_obj_t path;
    mp_obj_t name;
    mp_obj_t label;
    mp_obj_t *pins;
    int fd;
    uint32_t lines;
} gpio_port_t;

#define MP_IOCTL(fd, call, payload, msg) \
    { \
        MP_THREAD_GIL_EXIT(); \
        int result = ioctl(fd, call, payload); \
        MP_THREAD_GIL_ENTER(); \
        if (result < 0) { \
            raise_oserror(msg, errno); \
        } \
    }

#if MICROPY_PY_GPIO_IRQ

static void *gpio_epoll_thread(void *arg);
static pthread_t gpio_epoll_thread_id;
struct epoll_event gpio_epoll_events[MICROPY_PY_GPIO_IRQ_QUEUE_SIZE] = {};
static int gpio_epoll_fd = -1;
static volatile bool gpio_epoll_thread_stop = false;

#endif

static MP_NORETURN void raise_oserror(const char *text, int code) {
    if (text != NULL) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s: %d"), text, code);
    } else {
        mp_raise_OSError(code);
    }
}

static void fd_cleanup_callback(void *fd) {
    MP_THREAD_GIL_EXIT();
    close((intptr_t)fd);
    MP_THREAD_GIL_ENTER();
}

// Thanks to the existence of symbolic links, any character device on the
// machine can be referenced by an infinite number of names.  However,
// character devices can still be referenced by their device identifier.  Too
// bad a device identifier *is not unique* across devices of different classes
// so that won't save you from errors, and sending the right ioctl to the wrong
// device may or may not be harmful.  Caveat emptor.
//
// Still, the "/sys/bus/gpio/devices" directory will contain the names of
// available GPIO character devices, with each entry in there being a link to
// another directory with text files containing more information on the target
// device.  To keep things manageable and third party dependency free (as in,
// not making libudev a dependency), the given port name (not the full path!)
// is checked for existence as "/sys/bus/gpio/devices/<portname>", and if
// there's a match, then it is opened as "/dev/<portname>".  For the sake of
// security, characters in the port name can only be in the ranges "a-z",
// "A-Z", "0-9", "_", "-", and ".".  The lack of slashes, tildes, and other
// characters should be good enough for stopping unwanted directory traversals.
// If it does not, then patches are welcome.
//
// This method for resolving character devices' effective path, despite its
// limits, makes handling dynamic GPIO ports disconnections much easier.  An
// inotify change watch can be placed on "/sys/bus/gpio/devices" and then the
// ports dictionary can be updated to reflect the new devices set whenever the
// inotify callback is invoked.

#if MICROPY_PY_GPIO_IRQ

static void gpio_epoll_init(void) {
    mp_set_init(&MP_STATE_PORT(epoll_pins), 0);

    MP_THREAD_GIL_EXIT();
    gpio_epoll_fd = epoll_create1(0);
    MP_THREAD_GIL_ENTER();
    if (gpio_epoll_fd < 0) {
        raise_oserror("cannot create epoll fd", errno);
    }

    int result = pthread_create(&gpio_epoll_thread_id, NULL, &gpio_epoll_thread, (void *)&gpio_epoll_thread_stop);
    if (result != 0) {
        raise_oserror("cannot create epoll thread", result);
    }
}

static void gpio_epoll_deinit(void) {
    gpio_epoll_thread_stop = true;
    MP_THREAD_GIL_EXIT();
    close(gpio_epoll_fd);
    MP_THREAD_GIL_ENTER();
    gpio_epoll_fd = -1;
    int result = pthread_cancel(gpio_epoll_thread_id);
    assert(result >= 0 && "pthread_cancel failed to cancel the epoll thread");
    result = pthread_join(gpio_epoll_thread_id, NULL);
    assert(result >= 0 && "pthread_join failed to wait until the epoll thread exited");
    (void)result;
    // The pins set can be safely cleared without being in a critical section.
    mp_set_clear(&MP_STATE_PORT(epoll_pins));
}

// epoll() descriptor set management functions.

static void gpio_epoll_undo_addition(void *pin) {
    mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();
    mp_obj_t removed_object = mp_set_lookup(&MP_STATE_PORT(epoll_pins), MP_OBJ_FROM_PTR(pin), MP_MAP_LOOKUP_REMOVE_IF_FOUND);
    MICROPY_END_ATOMIC_SECTION(atomic_state);
    assert(removed_object != MP_OBJ_NULL && "Nothing to undo?");
    (void)removed_object;
}

static void gpio_epoll_add_pin(mp_obj_t pin_in) {
    machine_pin_obj_t *pin = MP_OBJ_TO_PTR(pin_in);

    assert((pin->fd >= 0) && "Adding a pin with an invalid descriptor to the epoll list");

    memset(&pin->event, 0x00, sizeof(struct epoll_event));
    // Only report edge-trigger events (we are just interested in whether the
    // pin changed state, nothing more).
    pin->event.events = EPOLLET;
    pin->event.data.ptr = pin;

    mp_set_t *epoll_pins = &MP_STATE_PORT(epoll_pins);

    int result = 0;
    mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();

    // There's no direct way to just query a set whether an element is stored
    // in there or not, so we traverse the items table instead.

    bool element_already_added = false;

    for (size_t index = 0; index < epoll_pins->alloc; index++) {
        mp_obj_t element = epoll_pins->table[index];
        if (element == pin_in) {
            element_already_added = true;
            break;
        }
    }

    assert((!element_already_added || (element_already_added && (pin->event.events != 0 && pin->event.data.ptr != 0))) && "epoll set went out of sync");

    if (!element_already_added) {
        // Adding the file descriptor to an epoll group descriptor may fail,
        // and if it does then we have to clean up the epoll pins set.
        // Updating the set may also incur on a faillible memory reallocation
        // too.  It's easier to undo a set addition rather than dealing with
        // epoll once more.
        //
        // TODO: are NLR jump callbacks safe to be fully enclosed inside a
        //       critical section?

        bool force_cleanup = false;
        MP_DEFINE_NLR_JUMP_CALLBACK_FUNCTION_1(ctx, gpio_epoll_undo_addition, (void *)pin);
        nlr_push_jump_callback(&ctx.callback, mp_call_function_1_from_nlr_jump_callback);

        mp_set_lookup(epoll_pins, pin_in, MP_MAP_LOOKUP_ADD_IF_NOT_FOUND);
        MP_THREAD_GIL_EXIT();
        result = epoll_ctl(gpio_epoll_fd, EPOLL_CTL_ADD, pin->fd, &pin->event);
        MP_THREAD_GIL_ENTER();
        if (result < 0) {
            force_cleanup = true;
        }

        nlr_pop_jump_callback(force_cleanup);
    }

    MICROPY_END_ATOMIC_SECTION(atomic_state);

    if (result < 0) {
        raise_oserror("cannot bind irq", -result);
    }
}

static void gpio_epoll_remove_pin(mp_obj_t pin_in) {
    // This may be invoked during GPIO subsystem deinitialisation, so if the
    // epoll thread should stop leave things alone (the group descriptor is
    // probably already closed by now).
    if (gpio_epoll_thread_stop) {
        return;
    }

    machine_pin_obj_t *pin = MP_OBJ_TO_PTR(pin_in);

    assert((pin->fd >= 0) && "Removing a pin with an invalid descriptor from the epoll group");
    memset(&pin->event, 0x00, sizeof(struct epoll_event));

    int result = 0;
    bool element_found = false;
    mp_set_t *epoll_pins = &MP_STATE_PORT(epoll_pins);
    mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();

    for (size_t index = 0; index < epoll_pins->alloc; index++) {
        mp_obj_t element = epoll_pins->table[index];
        if (element == pin_in) {
            element_found = true;
            break;
        }
    }

    if (element_found) {
        // Removing an item from a set will always succeed, so the only check here
        // is whether the removal of the pin's file descriptor from the epoll
        // descriptors group succeeded or not.

        MP_THREAD_GIL_EXIT();
        result = epoll_ctl(gpio_epoll_fd, EPOLL_CTL_DEL, pin->fd, NULL);
        MP_THREAD_GIL_ENTER();
        if (result >= 0) {
            mp_obj_t removed_object = mp_set_lookup(&MP_STATE_PORT(epoll_pins), MP_OBJ_FROM_PTR(pin), MP_MAP_LOOKUP_REMOVE_IF_FOUND);
            pin->callback = mp_const_none;
            assert(removed_object != MP_OBJ_NULL && "Pin descriptor not found in epoll group");
            (void)removed_object;
        }
    }

    MICROPY_END_ATOMIC_SECTION(atomic_state);

    if (result < 0) {
        raise_oserror("cannot unbind irq", -result);
    }
}

#endif

static mp_obj_t gpio_resolve_port_path(mp_obj_t port_in) {
    vstr_t name = {};

    if (mp_obj_is_int(port_in)) {
        mp_int_t device_node_id = mp_obj_get_int(port_in);
        // Artificially limit the number of GPIO ports to 255 for now.

        if (device_node_id < 0 || device_node_id > 255) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid device number"));
        }

        vstr_init(&name, sizeof("gpiochip"));
        vstr_add_str(&name, "gpiochip");

        bool emit_zeroes = false;
        if (device_node_id >= 100) {
            emit_zeroes = true;
            vstr_add_byte(&name, '0' + (device_node_id / 100));
            device_node_id %= 100;
        }
        if (device_node_id >= 10 || emit_zeroes) {
            vstr_add_byte(&name, '0' + (device_node_id / 10));
            device_node_id %= 10;
        }
        vstr_add_byte(&name, '0' + device_node_id);
    }

    if (mp_obj_is_str(port_in)) {
        size_t length = 0;
        const char *string = NULL;

        if (mp_obj_is_qstr(port_in)) {
            string = (const char *)qstr_data(MP_OBJ_QSTR_VALUE(port_in), &length);
        } else {
            string = mp_obj_str_get_data(port_in, &length);
        }

        for (size_t index = 0; index < length; index++) {
            char character = string[index];

            // "Be aware: These unichar_is* functions are actually ASCII-only!"
            if (!unichar_isalpha(character) && !unichar_isdigit(character) &&
                (character != '_') && (character != '-') && (character != '.')) {
                goto invalid_name;
            }
        }

        vstr_init(&name, length);
        vstr_add_str(&name, string);
    }

    if (vstr_len(&name) == 0) {
        goto invalid_name;
    }

    vstr_t path;
    vstr_init(&path, vstr_len(&name) + sizeof("/sys/bus/gpio/devices/"));
    vstr_add_str(&path, "/sys/bus/gpio/devices/");
    vstr_add_strn(&path, vstr_str(&name), vstr_len(&name));

    struct stat file_info;
    MP_THREAD_GIL_EXIT();
    int result = stat(vstr_null_terminated_str(&path), &file_info);
    MP_THREAD_GIL_ENTER();
    vstr_reset(&path);
    if (result < 0) {
        raise_oserror("cannot stat sysfs path", errno);
    }
    if ((file_info.st_mode & S_IFMT) != S_IFDIR) {
        raise_oserror("not a directory", MP_ENOTDIR);
    }

    vstr_add_str(&path, "/dev/");
    vstr_add_strn(&path, vstr_str(&name), vstr_len(&name));

    MP_THREAD_GIL_EXIT();
    result = stat(vstr_null_terminated_str(&path), &file_info);
    MP_THREAD_GIL_ENTER();
    if (result < 0) {
        raise_oserror("cannot stat dev path", errno);
    }
    if ((file_info.st_mode & S_IFMT) != S_IFCHR) {
        raise_oserror("not a char device", MP_EBADF);
    }
    mp_obj_t port_path = mp_obj_new_str_from_vstr(&path);

    return port_path;

invalid_name:
    mp_raise_TypeError(MP_ERROR_TEXT("invalid port name"));
}

// This expects to receive an already resolved port name!
static gpio_port_t *gpio_add_port(mp_obj_t path) {
    mp_map_t *ports = &MP_STATE_PORT(ports);
    assert(ports && "Ports dictionary must not be NULL");
    mp_map_elem_t *slot = mp_map_lookup(ports, path, MP_MAP_LOOKUP);
    if (slot) {
        return (gpio_port_t *)MP_OBJ_TO_PTR(slot->value);
    }

    MP_THREAD_GIL_EXIT();
    mp_int_t fd = open(mp_obj_str_get_str(path), O_RDWR | O_CLOEXEC);
    MP_THREAD_GIL_ENTER();
    if (fd < 0) {
        raise_oserror("cannot open port", fd);
    }

    MP_DEFINE_NLR_JUMP_CALLBACK_FUNCTION_1(ctx, fd_cleanup_callback, (void *)fd);
    nlr_push_jump_callback(&ctx.callback, mp_call_function_1_from_nlr_jump_callback);

    struct gpiochip_info info;
    memset(&info, 0x00, sizeof(info));
    MP_IOCTL(fd, GPIO_GET_CHIPINFO_IOCTL, &info, "cannot query port");

    mp_obj_t *pins = NULL;
    if (info.lines > 0) {
        // TODO: Is the assumption that MP_OBJ_NULL is equal to 0 always valid?
        //       If it is, then all of this could be replaced by
        //       "pins = m_new0(mp_obj_t, info.lines);"
        pins = m_new(mp_obj_t, info.lines);
        for (size_t index = 0; index < info.lines; index++) {
            pins[index] = MP_OBJ_NULL;
        }
    }

    gpio_port_t *port = m_new(gpio_port_t, 1);
    port->path = path;
    port->name = mp_obj_new_str_from_cstr(info.name);
    port->label = mp_obj_new_str_from_cstr(info.label);
    port->pins = pins;
    port->fd = fd;
    port->lines = info.lines;

    slot = mp_map_lookup(ports, path, MP_MAP_LOOKUP_ADD_IF_NOT_FOUND);
    assert(slot && "No slot to fill was returned on port add");
    slot->value = MP_OBJ_FROM_PTR(port);

    nlr_pop_jump_callback(false);

    return port;
}

// This expects to receive an already resolved port name!
static mp_obj_t gpio_find_pin(mp_obj_t port_key, uint32_t pin) {
    mp_map_t *ports = &MP_STATE_PORT(ports);
    mp_map_elem_t *slot = mp_map_lookup(ports, port_key, MP_MAP_LOOKUP);
    if (!slot || slot->value == MP_OBJ_NULL) {
        return MP_OBJ_NULL;
    }
    gpio_port_t *port = (gpio_port_t *)MP_OBJ_TO_PTR(slot->value);
    if (pin >= port->lines) {
        mp_raise_ValueError(MP_ERROR_TEXT("pin index out of range"));
    }
    return port->pins[pin];
}

static void gpio_evict_pin(mp_obj_t pin_in) {
    #if MICROPY_PY_GPIO_IRQ
    gpio_epoll_remove_pin(pin_in);
    #endif

    machine_pin_obj_t *pin = MP_OBJ_TO_PTR(pin_in);
    mp_map_t *ports = &MP_STATE_PORT(ports);
    mp_map_elem_t *slot = mp_map_lookup(ports, pin->port, MP_MAP_LOOKUP);
    if (!slot) {
        // The port this pin belongs to may have already been evicted during
        // GPIO support deinitialisation or by a pin object finaliser.
        return;
    }

    gpio_port_t *port = MP_OBJ_TO_PTR(slot->value);
    assert((port == (gpio_port_t *)MP_OBJ_TO_PTR(pin->port)) && "Pin and port went out of sync");
    assert((pin->number < port->lines) && "Pin and port went out of sync");
    assert(port->pins[pin->number] == pin_in && "Pin objects mismatch");
    port->pins[pin->number] = MP_OBJ_NULL;

    bool empty_port = true;
    for (size_t index = 0; index < port->lines; index++) {
        if (port->pins[index] != MP_OBJ_NULL) {
            empty_port = false;
            break;
        }
    }

    if (!empty_port) {
        return;
    }

    slot = mp_map_lookup(ports, pin->port, MP_MAP_LOOKUP_REMOVE_IF_FOUND);
    assert(slot && "Port disappeared from map");
    MP_THREAD_GIL_EXIT();
    close(port->fd);
    MP_THREAD_GIL_ENTER();
    port->fd = -1;
    m_del(mp_obj_t, port->pins, port->lines);
    slot->value = MP_OBJ_NULL;
    m_del(gpio_port_t, port, 1);
}

static uint32_t gpio_get_pin_number(mp_obj_t number_in) {
    assert(mp_obj_is_int(number_in) && "Invalid pin number type");

    uint32_t number = 0;
    bool valid = false;
    if (mp_obj_is_exact_type(number_in, &mp_type_int)) {
        #if MP_ENDIANNESS_BIG
        valid = mp_obj_int_to_bytes_impl(number_in, true, sizeof(uint32_t), (byte *)&number);
        #else
        valid = mp_obj_int_to_bytes_impl(number_in, false, sizeof(uint32_t), (byte *)&number);
        #endif
    } else {
        valid = MP_OBJ_SMALL_INT_VALUE(number_in) < 0xFFFFFFFFU;
        if (valid) {
            number = (uint32_t)MP_OBJ_SMALL_INT_VALUE(number_in);
        }
    }

    if (!valid) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid pin number"));
    }

    return number;
}

static void gpio_fill_line_config_struct(struct gpio_v2_line_config *config, uint32_t pin, mp_obj_t mode, mp_obj_t pull, mp_obj_t value) {
    assert(config && "GPIO config structrure must not be NULL");

    memset(config, 0x00, sizeof(struct gpio_v2_line_config));

    if (mode != mp_const_none) {
        switch (mp_obj_get_int(mode)) {
            case GPIO_V2_LINE_FLAG_INPUT:
                config->flags |= GPIO_V2_LINE_FLAG_INPUT;
                break;

            case GPIO_V2_LINE_FLAG_OUTPUT:
                config->flags |= GPIO_V2_LINE_FLAG_OUTPUT;
                break;

            default:
                mp_raise_ValueError(MP_ERROR_TEXT("invalid pin mode"));
                break;
        }
    }

    if (pull != MP_OBJ_NEW_SMALL_INT(-1)) {
        switch (mp_obj_get_int(pull)) {
            case GPIO_V2_LINE_FLAG_BIAS_PULL_UP:
                config->flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
                break;

            case GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN:
                config->flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
                break;

            default:
                mp_raise_ValueError(MP_ERROR_TEXT("invalid pull mode"));
                break;
        }
    }

    if (value != MP_OBJ_NULL) {
        config->num_attrs = 1;
        config->attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        config->attrs[0].attr.values = (mp_obj_is_true(value) ? 1U : 0U);
        config->attrs[0].mask = 1U;
    }
}

static int gpio_set_state_inner(int fd, mp_int_t state) {
    struct gpio_v2_line_values values;
    memset(&values, 0x00, sizeof(values));
    values.mask = 1U;
    values.bits = state != 0 ? 1 : 0;
    MP_THREAD_GIL_EXIT();
    int result = ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
    MP_THREAD_GIL_ENTER();
    return result;
}

static int gpio_get_state_inner(int fd, mp_int_t *state) {
    assert(state && "GPIO state value pointer cannot be NULL");

    struct gpio_v2_line_values values;
    memset(&values, 0x00, sizeof(values));
    values.mask = 1U;
    values.bits = 1U;
    MP_THREAD_GIL_EXIT();
    int result = ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values);
    MP_THREAD_GIL_ENTER();
    if (result >= 0) {
        *state = (values.bits & 1U) ? 1 : 0;
    }
    return result;
}

static void gpio_set_state(mp_obj_t self_in, mp_int_t state) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (gpio_set_state_inner(self->fd, state) < 0) {
        raise_oserror("cannot set pin value", errno);
    }
}

static bool gpio_get_state(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t state;
    if (gpio_get_state_inner(self->fd, &state) < 0) {
        raise_oserror("cannot get pin value", errno);
    }
    return !!state;
}

///////////////////////////////////////////////////////////////////////////////

static mp_obj_t init_helper(uint32_t pin_number, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_mode, ARG_pull, ARG_value, ARG_port, ARG_consumer };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode,                       MP_ARG_OBJ, { .u_obj = mp_const_none                        }},
        { MP_QSTR_pull,                       MP_ARG_OBJ, { .u_obj = MP_OBJ_NEW_SMALL_INT(-1)             }},
        { MP_QSTR_value,     MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL                          }},
        { MP_QSTR_port,      MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NEW_SMALL_INT(0)              }},
        { MP_QSTR_consumer,  MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NEW_QSTR(MP_QSTR_MicroPython) }},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    struct gpio_v2_line_config config;
    gpio_fill_line_config_struct(&config, pin_number, args[ARG_mode].u_obj, args[ARG_pull].u_obj, args[ARG_value].u_obj);

    mp_obj_t port_path = gpio_resolve_port_path(args[ARG_port].u_obj);

    mp_obj_t existing_pin = gpio_find_pin(port_path, pin_number);
    if (existing_pin != MP_OBJ_NULL) {
        machine_pin_obj_t *pin = MP_OBJ_TO_PTR(existing_pin);

        // Reconfigure the pin with the new parameters.
        // WARNING: If an IRQ callback was attached before this point, it will
        //          stay bound to the pin instance.
        if (pin->fd >= 0) {
            MP_IOCTL(pin->fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &config, "cannot set pin configuration");
            return existing_pin;
        }

        gpio_evict_pin(existing_pin);
    }

    if (!mp_obj_is_str(args[ARG_consumer].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("consumer must be a string"));
    }
    const char *consumer;
    size_t consumer_length;
    if (mp_obj_is_qstr(args[ARG_consumer].u_obj)) {
        consumer = (const char *)qstr_data(MP_OBJ_QSTR_VALUE(args[ARG_consumer].u_obj), &consumer_length);
    } else {
        consumer = mp_obj_str_get_data(args[ARG_consumer].u_obj, &consumer_length);
    }
    if (consumer_length >= GPIO_MAX_NAME_SIZE) {
        mp_raise_ValueError(MP_ERROR_TEXT("consumer name too long"));
    }

    // If the port is already known then it will just return the old instance.
    gpio_port_t *port = gpio_add_port(port_path);

    if (pin_number >= port->lines) {
        mp_raise_ValueError(MP_ERROR_TEXT("pin index out of range"));
    }

    struct gpio_v2_line_request request;
    memset(&request, 0x00, sizeof(request));
    request.offsets[0] = pin_number;
    request.config = config;
    request.num_lines = 1;
    strcpy(request.consumer, consumer);

    MP_IOCTL(port->fd, GPIO_V2_GET_LINE_IOCTL, &request, "cannot claim pin");

    MP_DEFINE_NLR_JUMP_CALLBACK_FUNCTION_1(ctx, fd_cleanup_callback, (void *)(intptr_t)request.fd);
    nlr_push_jump_callback(&ctx.callback, mp_call_function_1_from_nlr_jump_callback);

    machine_pin_obj_t *self = mp_obj_malloc_with_finaliser(machine_pin_obj_t, &machine_pin_type);
    self->port = MP_OBJ_FROM_PTR(port);
    self->number = pin_number;
    self->fd = request.fd;

    #if MICROPY_PY_GPIO_IRQ
    self->callback = mp_const_none;
    self->hard_callback = false;
    memset(&self->event, 0x00, sizeof(struct epoll_event));
    #endif

    mp_obj_t self_out = MP_OBJ_FROM_PTR(self);

    if (port->pins[pin_number] != MP_OBJ_NULL) {
        machine_pin_obj_t *old_self = MP_OBJ_TO_PTR(port->pins[pin_number]);
        gpio_evict_pin(port->pins[pin_number]);
        port->pins[pin_number] = MP_OBJ_NULL;
        MP_THREAD_GIL_EXIT();
        close(old_self->fd);
        MP_THREAD_GIL_ENTER();
    }
    port->pins[pin_number] = self_out;
    nlr_pop_jump_callback(false);

    return self_out;
}

mp_obj_t mp_pin_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    if (mp_obj_is_type(args[0], &machine_pin_type)) {
        return args[0];
    }

    if (mp_obj_is_int(args[0])) {
        uint32_t number = gpio_get_pin_number(args[0]);
        mp_map_t kw_args;
        mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
        return init_helper(number, n_args - 1, args + 1, &kw_args);
    }

    mp_raise_ValueError(MP_ERROR_TEXT("invalid pin type"));
}

static void machine_pin_obj_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const gpio_port_t *port = MP_OBJ_TO_PTR(self->port);
    mp_printf(print, "Pin(%s, %d)", mp_obj_str_get_str(port->name), self->number);
}

static mp_obj_t machine_pin_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (n_args == 0) {
        return MP_OBJ_NEW_SMALL_INT(gpio_get_state(self));
    }
    gpio_set_state(self, mp_obj_is_true(args[0]));
    return mp_const_none;
}

static mp_uint_t pin_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (self->fd < 0) {
        if (errcode != NULL) {
            *errcode = MP_EPIPE;
        }
        return -1;
    }

    switch (request) {
        case MP_PIN_READ: {
            mp_int_t state = 0;
            int result = gpio_get_state_inner(self->fd, &state);
            if (errcode != NULL) {
                *errcode = result < 0 ? result : 0;
            }
            return result < 0 ? -1 : state;
        }

        case MP_PIN_WRITE: {
            int result = gpio_set_state_inner(self->fd, arg != 0 ? 1 : 0);
            if (errcode != NULL) {
                *errcode = result;
            }
            return result < 0 ? -1 : 0;
        }

        default:
            return -1;
    }
}

static mp_obj_t machine_pin_off(mp_obj_t self_in) {
    gpio_set_state(self_in, 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_off_obj, machine_pin_off);

static mp_obj_t machine_pin_on(mp_obj_t self_in) {
    gpio_set_state(self_in, 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_on_obj, machine_pin_on);

static mp_obj_t machine_pin_toggle(mp_obj_t self_in) {
    gpio_set_state(self_in, gpio_get_state(self_in) ? 0 : 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_toggle_obj, machine_pin_toggle);

mp_obj_t machine_pin_del(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    #if MICROPY_PY_GPIO_IRQ
    gpio_epoll_remove_pin(self_in);
    #endif
    MP_THREAD_GIL_EXIT();
    close(self->fd);
    MP_THREAD_GIL_ENTER();
    self->fd = -1;
    gpio_evict_pin(self_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_del_obj, machine_pin_del);

static mp_obj_t machine_pin_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    return init_helper(gpio_get_pin_number(args[0]), n_args - 1, args + 1, kw_args);
}
MP_DEFINE_CONST_FUN_OBJ_KW(machine_pin_init_obj, 1, machine_pin_init);

static mp_obj_t machine_pin_available(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->fd != -1);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_available_obj, machine_pin_available);

mp_obj_t machine_pin_value(size_t n_args, const mp_obj_t *args) {
    return machine_pin_call(args[0], n_args - 1, 0, args + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_pin_value_obj, 1, 2, machine_pin_value);

#if MICROPY_PY_GPIO_IRQ

static mp_obj_t machine_pin_irq(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_handler, ARG_trigger, ARG_hard };

    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_handler, MP_ARG_OBJ,  { .u_obj = mp_const_none                                                  } },
        { MP_QSTR_trigger, MP_ARG_INT,  { .u_int = GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING } },
        { MP_QSTR_hard,    MP_ARG_BOOL, { .u_bool = false                                                         } },
    };

    machine_pin_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (n_args > 1 || kw_args->used != 0) {
        mp_uint_t trigger = args[ARG_trigger].u_int;
        if ((trigger & ~(GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING)) != 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid trigger"));
        }

        gpio_epoll_remove_pin(pos_args[0]);
        if (args[ARG_handler].u_obj != mp_const_none) {
            self->callback = args[ARG_handler].u_obj;
            self->hard_callback = args[ARG_hard].u_bool;
            gpio_epoll_add_pin(pos_args[0]);
        }
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_pin_irq_obj, 1, machine_pin_irq);

#endif

///////////////////////////////////////////////////////////////////////////////

#if MICROPY_PY_GPIO_IRQ
static void *gpio_epoll_thread(void *arg) {
    // TODO: Use io_uring instead?  That'd be more complex for sure but it'd be
    //       also faster.

    // Keep a reference to the stop variable.
    volatile bool *stop = (volatile bool *)arg;

    for (;;) {
        int events = epoll_wait(gpio_epoll_fd, &gpio_epoll_events[0], MP_ARRAY_SIZE(gpio_epoll_events), -1);
        if (stop) {
            return NULL;
        }

        if (events < 0) {
            // This is not correct but the code isn't in its final form yet.
            continue;
        }

        for (int event_index = 0; event_index < events; event_index++) {
            struct epoll_event *event = &gpio_epoll_events[event_index];
            assert(((event->events & EPOLLET) == EPOLLET) && "Event not marked as edge triggered");
            // TODO: Is this actually correct?  In theory this should work if
            //       all modifications to the pin object involve setting the
            //       relevant pointers to MP_OBJ_NULL, but then what happens if
            //       the IRQ handlers are collected?  This needs more thought.
            mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();
            mp_obj_t lookup_result = mp_set_lookup(&MP_STATE_PORT(epoll_pins), MP_OBJ_FROM_PTR(event->data.ptr), MP_MAP_LOOKUP);
            MICROPY_END_ATOMIC_SECTION(atomic_state);
            if (lookup_result == MP_OBJ_NULL) {
                continue;
            }
            machine_pin_obj_t *pin = MP_OBJ_TO_PTR(event->data.ptr);
            assert((pin->callback != MP_OBJ_NULL) && "Event for a tracked pin with no callback");
            if (pin->hard_callback) {
                mp_sched_lock();
                gc_lock();
                nlr_buf_t nlr;
                if (nlr_push(&nlr) == 0) {
                    mp_call_function_1(pin->callback, MP_OBJ_FROM_PTR(pin));
                    nlr_pop();
                } else {
                    // TODO: Handle uncaught exception
                }
                gc_unlock();
                mp_sched_unlock();
            } else {
                mp_sched_schedule(pin->callback, MP_OBJ_FROM_PTR(pin));
            }
        }
    }
}
#endif

void mp_pin_init(void) {
    mp_map_init(&MP_STATE_PORT(ports), 0);
    #if MICROPY_PY_GPIO_IRQ
    gpio_epoll_init();
    #endif
}

void mp_pin_deinit(void) {
    #if MICROPY_PY_GPIO_IRQ
    gpio_epoll_deinit();
    #endif

    mp_map_t *ports = &MP_STATE_PORT(ports);
    if (ports->table == NULL) {
        return;
    }

    for (size_t port_index = 0; port_index < ports->alloc; port_index++) {
        if (!mp_map_slot_is_filled(ports, port_index)) {
            continue;
        }

        mp_map_elem_t slot = ports->table[port_index];
        gpio_port_t *port = MP_OBJ_TO_PTR(slot.value);
        close(port->fd);
        if (port->lines > 0) {
            for (size_t pin = 0; pin < port->lines; pin++) {
                if (port->pins[pin] != MP_OBJ_NULL) {
                    machine_pin_del(port->pins[pin]);
                    port->pins[pin] = MP_OBJ_NULL;
                }
            }
            m_del(mp_obj_t, port->pins, port->lines);
            port->pins = NULL;
        }
        assert(port->pins == NULL && "GPIO port count went out of sync");
        m_del_obj(gpio_port_t, port);
        slot.value = MP_OBJ_NULL;
    }
    mp_map_clear(ports);

    // Root pointers will be freed by the OS after this point.  For the Unix
    // port, once the main interpreter loop exits there's no recovery unlike
    // on QEMU or other microcontroller-based ports where the board would just
    // reboot and restart things up.
}

static const mp_rom_map_elem_t machine_pin_locals_dict_table[] = {
    // instance methods

    { MP_ROM_QSTR(MP_QSTR___del__),     MP_ROM_PTR(&machine_pin_del_obj)             },

    { MP_ROM_QSTR(MP_QSTR_init),        MP_ROM_PTR(&machine_pin_init_obj)            },

    { MP_ROM_QSTR(MP_QSTR_value),       MP_ROM_PTR(&machine_pin_value_obj)           },
    { MP_ROM_QSTR(MP_QSTR_off),         MP_ROM_PTR(&machine_pin_off_obj)             },
    { MP_ROM_QSTR(MP_QSTR_on),          MP_ROM_PTR(&machine_pin_on_obj)              },
    { MP_ROM_QSTR(MP_QSTR_toggle),      MP_ROM_PTR(&machine_pin_toggle_obj)          },

    #if MICROPY_PY_GPIO_IRQ
    { MP_ROM_QSTR(MP_QSTR_irq),         MP_ROM_PTR(&machine_pin_irq_obj)             },
    #endif

    { MP_ROM_QSTR(MP_QSTR_close),       MP_ROM_PTR(&machine_pin_del_obj)             },
    { MP_ROM_QSTR(MP_QSTR_available),   MP_ROM_PTR(&machine_pin_available_obj)       },
    { MP_ROM_QSTR(MP_QSTR___enter__),   MP_ROM_PTR(&mp_identity_obj)                 },
    { MP_ROM_QSTR(MP_QSTR___exit__),    MP_ROM_PTR(&machine_pin_del_obj)             },

    // class constants

    { MP_ROM_QSTR(MP_QSTR_IN),          MP_ROM_INT(GPIO_V2_LINE_FLAG_INPUT)          },
    { MP_ROM_QSTR(MP_QSTR_OUT),         MP_ROM_INT(GPIO_V2_LINE_FLAG_OUTPUT)         },
    { MP_ROM_QSTR(MP_QSTR_OPEN_DRAIN),  MP_ROM_INT(GPIO_V2_LINE_FLAG_OPEN_DRAIN)     },
    { MP_ROM_QSTR(MP_QSTR_PULL_UP),     MP_ROM_INT(GPIO_V2_LINE_FLAG_BIAS_PULL_UP)   },
    { MP_ROM_QSTR(MP_QSTR_PULL_DOWN),   MP_ROM_INT(GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN) },

    #if MICROPY_PY_GPIO_IRQ
    { MP_ROM_QSTR(MP_QSTR_IRQ_RISING),  MP_ROM_INT(GPIO_V2_LINE_FLAG_EDGE_RISING)    },
    { MP_ROM_QSTR(MP_QSTR_IRQ_FALLING), MP_ROM_INT(GPIO_V2_LINE_FLAG_EDGE_FALLING)   },
    #endif
};

static MP_DEFINE_CONST_DICT(machine_pin_locals_dict, machine_pin_locals_dict_table);

static const mp_pin_p_t machine_pin_obj_protocol = {
    .ioctl = pin_ioctl,
};

MP_DEFINE_CONST_OBJ_TYPE(
    machine_pin_type,
    MP_QSTR_Pin,
    MP_TYPE_FLAG_NONE,
    make_new, mp_pin_make_new,
    print, machine_pin_obj_print,
    call, machine_pin_call,
    protocol, &machine_pin_obj_protocol,
    locals_dict, &machine_pin_locals_dict
    );

MP_REGISTER_ROOT_POINTER(mp_map_t ports);

#if MICROPY_PY_GPIO_IRQ
MP_REGISTER_ROOT_POINTER(mp_set_t epoll_pins);
#endif

#endif

// vim:et:sw=4:ts=4:foldmethod=syntax:cc=80:
