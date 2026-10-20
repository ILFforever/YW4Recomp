// Golden-result harness: runs ARM64 test instructions natively on the Pi 5.
//
// Input (stdin), one case per line, all hex:
//     <encoding> <x0> ... <x30> <nzcv>
// Output (stdout), one line per case:
//     <encoding> <x0> ... <x30> <nzcv>      (state after the instruction)
//
// Build on the Pi: gcc -O1 -o harness harness.c trampoline.S

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef struct {
    uint64_t x[31];
    uint64_t nzcv;
} TestState;

extern const uint32_t tramp_start[], tramp_slot[], tramp_end[];

int main(void) {
    size_t len = (size_t)((const char *)tramp_end - (const char *)tramp_start);
    size_t slot = (size_t)(tramp_slot - tramp_start);
    uint32_t *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memcpy(page, tramp_start, len);
    void (*run)(TestState *) = (void (*)(TestState *))page;

    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        TestState s;
        uint32_t enc;
        char *p = line;
        enc = (uint32_t)strtoul(p, &p, 16);
        for (int i = 0; i < 31; i++) s.x[i] = strtoull(p, &p, 16);
        s.nzcv = strtoull(p, &p, 16);

        page[slot] = enc;
        __builtin___clear_cache((char *)page, (char *)page + len);
        run(&s);

        printf("%08" PRIx32, enc);
        for (int i = 0; i < 31; i++) printf(" %" PRIx64, s.x[i]);
        printf(" %" PRIx64 "\n", s.nzcv);
    }
    return 0;
}
