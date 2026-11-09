/* csrng: the system's random numbers (Switchbrew "SPL services", csrng). Local play asks it when a room is made
 * (2026-11-10: "no native service csrng"): Pia's session keys. Filled from the host's cryptographic generator. */
#define _CRT_RAND_S
#include <stdlib.h>
#include <string.h>

#include "ipc_simple.h"

static void random_bytes(uint8_t *p, size_t n) {
    while (n) {
        unsigned int v = 0;
#ifdef _WIN32
        rand_s(&v);
#else
        v = (unsigned int)arc4random();
#endif
        size_t k = n < sizeof v ? n : sizeof v;
        memcpy(p, &v, k);
        p += k, n -= k;
    }
}

void svc_csrng(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 0) {  /* GetRandomBytes(out buffer) */
        IpcBuffer out = ipc_out_buffer(req, 0);
        if (out.addr && out.size) random_bytes(GPTR(out.addr), out.size);
        return;
    }
    ipc_unimplemented(s, req, resp);
}
