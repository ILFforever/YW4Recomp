/* nn::vi, the display, as overrides of the sdk's functions (recomp/overrides.toml, decision 0017).
 *
 * On the Switch, NVN's window presents through a vi layer and the binder buffer queue
 * behind it. Our NVN presents itself, so the display and layer here are just objects to
 * hand back, and the native window is a token nvnWindowBuilderSetNativeWindow receives and
 * ignores. What matters is the vsync event: a host thread signals it at 60 Hz, and the
 * game paces its frames on it. Approach as in LAS Recomp's hle/vi.c (MIT, third_party/lasrecomp).
 */
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "kernel.h"
#include "yw4load.h"

#define VSYNC_PERIOD_NS 16666667  /* 60 Hz */

static void *token(void) {
    void *p = calloc(1, 0x100);
    if (!p) exit(3);
    return p;
}

void yw4_vi_Initialize(CPUState *c) { (void)c; }
void yw4_vi_Finalize(CPUState *c) { (void)c; }

/* OpenDefaultDisplay(Display **out) -> Result */
void yw4_vi_OpenDefaultDisplay(CPUState *c) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)(uintptr_t)token();
    c->x[0] = 0;
}
void yw4_vi_CloseDisplay(CPUState *c) { (void)c; }

/* CreateLayer(Layer **out, Display *, int width, int height) -> Result */
void yw4_vi_CreateLayer(CPUState *c) {
    fprintf(stderr, "yw4: vi: layer %dx%d\n", (int)c->x[2], (int)c->x[3]);
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)(uintptr_t)token();
    c->x[0] = 0;
}
void yw4_vi_DestroyLayer(CPUState *c) { (void)c; }

/* GetNativeWindow(void **out, Layer *) -> Result */
void yw4_vi_GetNativeWindow(CPUState *c) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)(uintptr_t)token();
    c->x[0] = 0;
}

/* ---- vsync */

static KEvent *vsync;

static void vsync_thread(void *arg) {
    (void)arg;
    int64_t next = host_now_ns();
    for (;;) {
        next += VSYNC_PERIOD_NS;
        int64_t now = host_now_ns();
        if (next > now) yw4_host_sleep_ns(next - now);
        else if (now - next > 3 * VSYNC_PERIOD_NS) next = now;  /* fell far behind: don't burst to catch up */
        yw4_event_signal_from_host(vsync);
    }
}

/* GetDisplayVsyncEvent(nn::os::SystemEventType *event, Display *) -> Result. The sdk's
 * version attaches a readable event handle to the game's SystemEventType; so does this one,
 * by calling nn::os::AttachReadableHandleToSystemEvent(event, handle, managed = false,
 * clear mode = auto clear). */
void yw4_vi_GetDisplayVsyncEvent(CPUState *c) {
    uint32_t handle;
    yw4_kernel_lock();
    if (!vsync) {
        vsync = yw4_event_new(&handle);
        vsync->host_signalled = 1;
        if (host_thread_start(vsync_thread, NULL, 1 << 16)) {
            fprintf(stderr, "yw4: vi: cannot start the vsync thread\n");
            exit(3);
        }
    } else {
        handle = yw4_handle_new(&vsync->obj);
    }
    yw4_kernel_unlock();
    uint64_t lr = c->x[30];
    c->x[1] = handle, c->x[2] = 0, c->x[3] = 1;  /* x0 is already the event */
    yw4_resolve(yw4_guest_symbol("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"))(c);
    c->x[30] = lr;
    c->x[0] = 0;
}
