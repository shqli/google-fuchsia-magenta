#define _ALL_SOURCE
#include "libc.h"
#include <stdint.h>
#include <stdlib.h>
#include <threads.h>

void* __dso_handle = NULL;

/* Ensure that at least 32 atexit handlers can be registered without malloc */
#define COUNT 32

static struct fl {
    struct fl* next;
    void (*f[COUNT])(void*);
    void* a[COUNT];
} builtin, *head;

static int slot;
static mtx_t lock = MTX_INIT;

void __funcs_on_exit(void) {
    void (*func)(void*), *arg;
    mtx_lock(&lock);
    for (; head; head = head->next, slot = COUNT)
        while (slot-- > 0) {
            func = head->f[slot];
            arg = head->a[slot];
            mtx_unlock(&lock);
            func(arg);
            mtx_lock(&lock);
        }
}

void __cxa_finalize(void* dso) {}

int __cxa_atexit(void (*func)(void*), void* arg, void* dso) {
    mtx_lock(&lock);

    /* Defer initialization of head so it can be in BSS */
    if (!head)
        head = &builtin;

    /* If the current function list is full, add a new one */
    if (slot == COUNT) {
        struct fl* new_fl = calloc(sizeof(struct fl), 1);
        if (!new_fl) {
            mtx_unlock(&lock);
            return -1;
        }
        new_fl->next = head;
        head = new_fl;
        slot = 0;
    }

    /* Append function to the list. */
    head->f[slot] = func;
    head->a[slot] = arg;
    slot++;

    mtx_unlock(&lock);
    return 0;
}

static void call(void* p) {
    ((void (*)(void))(uintptr_t)p)();
}

int atexit(void (*func)(void)) {
    return __cxa_atexit(call, (void*)(uintptr_t)func, __dso_handle);
}