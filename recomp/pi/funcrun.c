// Runs whole test functions (tests/functest/) natively on the ARM64 reference machine.
//
//   funcrun <image.bin>
//
// Input line (hex):   <function offset> <a0> <a1> <a2> <a3> <inmask>
// Output line (hex):  <function offset> <x0 result> <memory changes>
//
// The image is the flat test image (code at offset 0). Arguments set in inmask are
// offsets into the scratch buffer and get its address added (testcase.h).
//
// Build on the Pi: gcc -O1 -o funcrun funcrun.c

#include <sys/mman.h>

#include "testcase.h"

typedef uint64_t (*TestFn)(uint64_t, uint64_t, uint64_t, uint64_t);

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: funcrun <image.bin>\n");
        return 1;
    }
    FILE *img = fopen(argv[1], "rb");
    if (!img) {
        perror(argv[1]);
        return 1;
    }
    size_t cap = 1 << 22;
    uint8_t *base = mmap(NULL, cap, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    size_t len = fread(base, 1, cap, img);
    fclose(img);
    __builtin___clear_cache((char *)base, (char *)base + len);
    test_mem_init();
    uint64_t mem = (uint64_t)(uintptr_t)test_mem;

    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        char *p = line;
        uint64_t off = strtoull(p, &p, 16), a[4];
        for (int i = 0; i < 4; i++) a[i] = strtoull(p, &p, 16);
        uint64_t inmask = strtoull(p, &p, 16);
        for (int i = 0; i < 4; i++)
            if (inmask >> i & 1) a[i] += mem;
        uint64_t r = ((TestFn)(base + off))(a[0], a[1], a[2], a[3]);
        printf("%" PRIx64 " %" PRIx64 " ", off, r);
        test_write_mem(stdout);
    }
    return 0;
}
