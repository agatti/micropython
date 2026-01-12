/*
 * This file is part of the MicroPython project, https://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Alessandro Gatti
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

#include "py/mpstate.h"
#include "py/runtime.h"

#include <windows.h>
#include <memoryapi.h>
#include <sysinfoapi.h>

#if MICROPY_ENABLE_NATIVE_CODE

// The memory allocated here is not on the GC heap (and it may contain pointers
// that need to be GC'd) so we must somehow trace this memory.  We do it by
// keeping a linked list of all mmap'd regions, and tracing them explicitly.

typedef struct _mmap_region_t {
    void *ptr;
    size_t len;
    struct _mmap_region_t *next;
} mmap_region_t;

void mp_windows_alloc_exec(size_t min_size, void **ptr, size_t *size) {
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    // The effective size will be rounded up to the nearest
    // SYSTEM_INFO.dwPageSize unit.
    *ptr = VirtualAlloc(NULL, min_size, MEM_COMMIT, PAGE_READWRITE);
    if (!ptr) {
        mp_raise_type(&mp_type_MemoryError);
    }
    *size = ((min_size + (system_info.dwPageSize - 1)) / system_info.dwPageSize) * system_info.dwPageSize;

    // add new link to the list of mmap'd regions
    mmap_region_t *rg = m_new_obj(mmap_region_t);
    rg->ptr = *ptr;
    rg->len = *size;
    rg->next = MP_STATE_VM(mmap_region_head);
    MP_STATE_VM(mmap_region_head) = rg;
}

// This is needed to keep compatibility with Windows XP/Windows 2003 systems.
// On newer systems VirtualAlloc can mark a page readable, writable, and
// executable in one single step, whilst on XP/2003 this has to be done in two
// steps and after code has been written to the buffer.

void mp_windows_mark_exec(void *ptr, size_t size) {
    DWORD unused;
    VirtualProtect(ptr, size, PAGE_EXECUTE_READ, &unused);
}

void mp_windows_free_exec(void *ptr, size_t size) {
    (void)size;
    BOOL freed = VirtualFree(ptr, 0, MEM_RELEASE);
    assert(freed && "Cannot free allocated memory");

    // unlink the mmap'd region from the list
    for (mmap_region_t **rg = (mmap_region_t **)&MP_STATE_VM(mmap_region_head); *rg != NULL; *rg = (*rg)->next) {
        if ((*rg)->ptr == ptr) {
            mmap_region_t *next = (*rg)->next;
            m_del_obj(mmap_region_t, *rg);
            *rg = next;
            return;
        }
    }
}

MP_REGISTER_ROOT_POINTER(void *mmap_region_head);

#endif // MICROPY_ENABLE_NATIVE_CODE
