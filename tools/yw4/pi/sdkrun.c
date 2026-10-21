/* Runs functions of the owner's sdk natively on the ARM64 reference machine (decision 0013).
 *
 *   sdkrun sdk.bin relocs.bin < cases > results
 *
 * sdk.bin is the sdk's flat image (from the owner's own dump, never committed). relocs.bin
 * lists the pointer slots to fill, prepared by tools/yw4/sdkref.py: little-endian records of
 * (u64 slot offset, u64 target offset, u64 kind), kind 0 = image base + target,
 * kind 1 = an import the sdk takes from another module: points at a stop function.
 *
 * Each input line is "<signature> <function offset hex> <argument bits hex>...", and each
 * output line holds the result bits in hex (plus the second result for frexp/modf/modff).
 * Only self-contained leaf-style functions make sense here: nothing that needs threads,
 * TLS or other modules.
 *
 * The Pi's FPCR is 0 at start-up, like every game thread on the Switch (decision 0008).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static void unresolved_import(void) {
    fprintf(stderr, "sdkrun: the function called an import from another module\n");
    exit(2);
}

static uint8_t *load(const char *image_path, const char *reloc_path, size_t mem_size) {
    FILE *f = fopen(image_path, "rb");
    if (!f) return NULL;
    uint8_t *base = mmap(NULL, mem_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;
    size_t n = fread(base, 1, mem_size, f);
    fclose(f);
    if (!n) return NULL;
    FILE *r = fopen(reloc_path, "rb");
    if (!r) return NULL;
    uint64_t rec[3];
    while (fread(rec, 8, 3, r) == 3) {
        uint64_t v = rec[2] == 0 ? (uint64_t)(uintptr_t)base + rec[1] : (uint64_t)(uintptr_t)unresolved_import;
        memcpy(base + rec[0], &v, 8);
    }
    fclose(r);
    __builtin___clear_cache((char *)base, (char *)base + mem_size);
    return base;
}

static double d_of(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static float f_of(uint64_t b) { float f; uint32_t u = (uint32_t)b; memcpy(&f, &u, 4); return f; }
static uint64_t bits_d(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static uint64_t bits_f(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: sdkrun sdk.bin relocs.bin <memory size>\n");
        return 1;
    }
    uint8_t *base = load(argv[1], argv[2], strtoull(argv[3], NULL, 0));
    if (!base) {
        perror("sdkrun: load");
        return 1;
    }
    char sig[16];
    uint64_t off, a, b;
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        a = b = 0;
        if (sscanf(line, "%15s %" SCNx64 " %" SCNx64 " %" SCNx64, sig, &off, &a, &b) < 3) continue;
        void *fn = base + off;
        if (!strcmp(sig, "dd")) {
            printf("%016" PRIx64 "\n", bits_d(((double (*)(double))fn)(d_of(a))));
        } else if (!strcmp(sig, "ddd")) {
            printf("%016" PRIx64 "\n", bits_d(((double (*)(double, double))fn)(d_of(a), d_of(b))));
        } else if (!strcmp(sig, "ff")) {
            printf("%08" PRIx64 "\n", bits_f(((float (*)(float))fn)(f_of(a))));
        } else if (!strcmp(sig, "fff")) {
            printf("%08" PRIx64 "\n", bits_f(((float (*)(float, float))fn)(f_of(a), f_of(b))));
        } else if (!strcmp(sig, "frexp")) {
            int e = 0x7eadbeef;
            double r = ((double (*)(double, int *))fn)(d_of(a), &e);
            printf("%016" PRIx64 " %08x\n", bits_d(r), (unsigned)e);
        } else if (!strcmp(sig, "ldexp")) {
            printf("%016" PRIx64 "\n", bits_d(((double (*)(double, int))fn)(d_of(a), (int)(int32_t)b)));
        } else if (!strcmp(sig, "modf")) {
            double ip = 0;
            double r = ((double (*)(double, double *))fn)(d_of(a), &ip);
            printf("%016" PRIx64 " %016" PRIx64 "\n", bits_d(r), bits_d(ip));
        } else if (!strcmp(sig, "modff")) {
            float ip = 0;
            float r = ((float (*)(float, float *))fn)(f_of(a), &ip);
            printf("%08" PRIx64 " %08" PRIx64 "\n", bits_f(r), bits_f(ip));
        } else {
            fprintf(stderr, "sdkrun: unknown signature %s\n", sig);
            return 1;
        }
    }
    return 0;
}
