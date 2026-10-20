// Golden-result harness: runs ARM64 test instructions natively on the Pi 5.
//
// Reads test cases on stdin and writes results on stdout, one line each
// (format and memory handling: testcase.h).
//
// Build on the Pi: gcc -O1 -o harness harness.c trampoline.S

#include <sys/mman.h>

#include "testcase.h"

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
    test_mem_init();
    uint64_t base = (uint64_t)(uintptr_t)test_mem;

    TestState s;
    uint32_t enc;
    uint64_t inmask, outmask;
    while (test_read(stdin, &enc, &s, &inmask, &outmask)) {
        page[slot] = enc;
        __builtin___clear_cache((char *)page, (char *)page + len);
        test_rebase(&s, inmask, base);
        run(&s);
        test_rebase(&s, outmask, 0 - base);
        test_write(stdout, enc, &s);
    }
    return 0;
}
