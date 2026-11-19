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

#include "hle.h"

#define R_OK 0u
/* nn::ssl is result module 123; a failure the game treats like any failed connection set-up */
#define R_SSL_NO_CONTEXT (123u | (1u << 9))

static void logged(const char *what) {
    static unsigned shown;
    if (shown++ < 8) fprintf(stderr, "yw4: ssl: %s (internet play: no secure connections)\n", what);
}

void yw4_ssl_Initialize(CPUState *c) { logged("Initialize"), c->x[0] = R_OK; }
void yw4_ssl_Finalize(CPUState *c) { logged("Finalize"), c->x[0] = R_OK; }
void yw4_ssl_ContextCreate(CPUState *c) { logged("Context::Create refused"), c->x[0] = R_SSL_NO_CONTEXT; }
void yw4_ssl_ContextDestroy(CPUState *c) { c->x[0] = R_OK; }
