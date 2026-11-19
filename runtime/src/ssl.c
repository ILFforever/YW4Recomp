/* nn::ssl, native (decision 0082): the game's internet mode never gets a secure connection.
 *
 * The game's own "Online" (internet) menu starts Nintendo's ssl library before it asks for a network; the sdk's
 * nnsslInitialize opens the ssl system service, which we don't have, and the run stopped there (2026-11-19, the
 * owner's first click on Online). Internet play needs Nintendo's NEX servers, which are gone (playing together
 * goes through local play and our relay instead), so: the library starts and stops without a service, and a
 * context can't be made. Every other ssl call needs a context, so none of them is reached. After this the game
 * asks for its network (nifm, ldn.c: an internet request is never granted) and shows its own "can't connect".
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "kernel.h"
#include "relay.h"

/* ---- recon of the game's Online mode (2026-11-19: where it splits from local play, and what it asks for when it
 * gets further). YW4_NET_TRACE=1: the guest call stack the first time each mode starts (ssl for Online, ldn for
 * local play); compared, the two show where the menu chooses. YW4_FAKE_INTERNET=1: the internet request is
 * granted (ldn.c's nifm), so the log shows the calls after it until something stops it. Tests only. */
int yw4_net_trace_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("YW4_NET_TRACE"); v = e && !strcmp(e, "1"); }
    return v;
}
int yw4_fake_internet(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("YW4_FAKE_INTERNET"); v = e && !strcmp(e, "1"); }
    return v;
}
void yw4_net_trace(const CPUState *c, const char *what) {
    if (!yw4_net_trace_on()) return;
    uint64_t st[16];
    char where[48];
    int n = yw4_call_stack(c, st, 16);
    fprintf(stderr, "yw4: net trace: %s, called from %s\n", what, yw4_describe(c->x[30] - 4, where));
    for (int i = 0; i < n; i++) fprintf(stderr, "       %s\n", yw4_describe(st[i] - 4, where));
}

#define R_OK 0u
/* nn::ssl is result module 123; a failure the game treats like any failed connection set-up */
#define R_SSL_NO_CONTEXT (123u | (1u << 9))

static void logged(const char *what) {
    static unsigned shown;
    if (shown++ < 8) fprintf(stderr, "yw4: ssl: %s (internet play: no secure connections)\n", what);
}

void yw4_ssl_Initialize(CPUState *c) {
    static int traced;
    if (!traced++) yw4_net_trace(c, "ssl Initialize (the game's Online mode)");
    logged("Initialize"), c->x[0] = R_OK;
}
void yw4_ssl_Finalize(CPUState *c) { logged("Finalize"), c->x[0] = R_OK; }
void yw4_ssl_ContextCreate(CPUState *c) { logged("Context::Create refused"), c->x[0] = R_SSL_NO_CONTEXT; }
void yw4_ssl_ContextDestroy(CPUState *c) { c->x[0] = R_OK; }
