/* YW4_TRACE_AI=1: who in the game evaluates the battle AI's conditions (debugging only).
 *
 * The AI tables (gamedata/battle/ai/..._ai.cfg.bin: party_ai, enemy_ai, ...) hold their conditions as small encoded
 * expressions (base64 in the file): "00 00 00 00 <size> <count>", then e.g. 35 <u32 function hash> ... 32 <u32 constant>
 * ... 78 (a comparison). A background thread looks for one of party_ai's expressions in the game's memory every 3
 * seconds, decoded first, else as the file's base64 text, and puts a read watch (host.c) on the first copy found: the
 * reads are reported with the reader's guest call stack (kernel.c). That names the evaluator, the next step to reading
 * the AI's rules. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "kernel.h"
#include "yw4load.h"

#ifdef _WIN32
#include <windows.h>

/* party_ai's condition 23 (an HP check, the hash 0x4e97c967 then 0x8996b8e1 and a constant 300): decoded and as base64 */
static const uint8_t EXPR[] = {0x00, 0x00, 0x00, 0x00, 0x1e, 0x0b, 0x35, 0x67, 0xc9, 0x97, 0x4e, 0x00, 0x01, 0x00, 0x32,
                               0x00, 0x00, 0x00, 0x00, 0x78, 0x35, 0xe1, 0xb8, 0x96, 0x89, 0x00, 0x01, 0x00, 0x32,
                               0x00, 0x00, 0x01, 0x2c, 0x71, 0x8f};
static const char EXPR64[] = "AAAAAB4LNWfJl04AAQAyAAAAAHg14biWiQABADIAAAEscY8=";

/* The first copy of `pat` in the game's committed memory, or 0. */
static uint64_t find(const void *pat, size_t n) {
    for (uint64_t a = YW4_GUEST_LO; a < YW4_GUEST_HI;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) break;
        uint64_t base = (uint64_t)(uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY)) && !(mbi.Protect & PAGE_GUARD)) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)base, *e = p + mbi.RegionSize;
            for (; p + n <= e; p++) {
                p = memchr(p, ((const uint8_t *)pat)[0], (size_t)(e - p - n + 1));
                if (!p) break;
                if (!memcmp(p, pat, n)) return (uint64_t)(uintptr_t)p;
            }
        }
        a = end > a ? end : a + 4096;
    }
    return 0;
}

static void search(void *arg) {
    (void)arg;
    for (int round = 0; round < 200; round++) {  /* 10 minutes */
        Sleep(3000);
        /* the expression's core from its first call on (the game may keep it without the 4-byte header and the size) */
        uint64_t at = find(EXPR + 6, 19);
        int decoded = at != 0;
        if (!at && round >= 20) at = find(EXPR64, sizeof EXPR64 - 1);  /* after a minute: the base64 text */
        if (!at) continue;
        fprintf(stderr, "yw4: ai: party_ai's condition 23 found %s at %#llx\n", decoded ? "decoded" : "as base64 text",
                (unsigned long long)at);
        yw4_read_watch_arm(at, decoded ? 19 : sizeof EXPR64 - 1, 40);
        return;
    }
    fprintf(stderr, "yw4: ai: party_ai's condition 23 not found in memory\n");
}

void ai_trace_start(void) {
    const char *e = getenv("YW4_TRACE_AI");
    if (!e || *e != '1') return;
    fprintf(stderr, "yw4: ai: looking for the battle AI's conditions in memory (YW4_TRACE_AI)\n");
    host_thread_start(search, NULL, 1 << 20);
}
#else
void ai_trace_start(void) {}
#endif
