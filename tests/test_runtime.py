"""Tests for runtime C code that don't need the game: compiled with the host compiler."""

from __future__ import annotations

import hashlib
import os
import random
import subprocess
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

from recomp import check

RUNTIME = check.REPO / "runtime"
# glibc hides the POSIX names the runtime uses under -std=c11 (recomp/build.py passes the same off Windows)
POSIX = [] if sys.platform == "win32" else ["-D_GNU_SOURCE"]


def sdl3(folder: Path) -> tuple[list[str], list[str]] | None:
    """SDL3 for a test program built in `folder`: (compiler flags, what to link), or None when this system has
    none (the test is skipped). Windows: the pinned package recomp.build uses, in the owner's work folder when
    there is a config, else a folder of its own under the temp folder (GitHub has no config); its DLL is copied
    next to the program. Elsewhere: the system's, by pkg-config."""
    if sys.platform != "win32":
        try:
            pc = lambda *a: subprocess.run(["pkg-config", *a, "sdl3"], capture_output=True, text=True,
                                           check=True).stdout.split()
            return pc("--cflags"), pc("--libs")
        except (subprocess.CalledProcessError, FileNotFoundError):
            return None
    from types import SimpleNamespace
    from tools.yw4 import config, deps
    try:
        cfg = config.load(require_exefs=False)
    except config.ConfigError:
        cfg = SimpleNamespace(work_dir=Path(tempfile.gettempdir()) / "yw4_test_deps")
    sdl = deps.fetch_sdl(cfg)
    shutil.copy2(sdl / "bin" / "SDL3.dll", folder / "SDL3.dll")
    return [f"-I{sdl / 'include'}"], [str(sdl / "lib" / "libSDL3.dll.a")]

SHA_DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include "yw4load.h"
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) return 1;
    char out[65];
    yw4_sha256_hex(b, (size_t)n, out);
    puts(out);
    return 0;
}
"""


class Sha256Test(unittest.TestCase):
    """The runtime's version lock hashes main.bin itself; it must agree with hashlib."""

    def test_matches_hashlib(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "drv.c").write_text(SHA_DRIVER)
            exe = t / "sha.exe"
            subprocess.run([check.host_cc(), "-std=c11", "-O1", f"-I{RUNTIME / 'include'}", "-o", str(exe),
                            str(t / "drv.c"), str(RUNTIME / "src" / "sha256.c")], check=True, capture_output=True)
            rng = random.Random(1)
            # lengths around the 55/56/64-byte padding boundaries, plus a big one
            for n in (0, 1, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 1000, 100_003):
                data = bytes(rng.getrandbits(8) for _ in range(n))
                (t / "in.bin").write_bytes(data)
                got = subprocess.run([str(exe), str(t / "in.bin")], capture_output=True, text=True, check=True)
                self.assertEqual(got.stdout.strip(), hashlib.sha256(data).hexdigest(), f"length {n}")


HEAP_DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#define REGION (64u << 20)
#define SLOTS 4000
static unsigned long long rng = 88172645463325252ull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)rng; }
static struct { unsigned char *p; size_t n; unsigned char fill; } s[SLOTS];
#define FAIL(...) do { printf(__VA_ARGS__); puts(""); return 1; } while (0)
static int check_live(void) {  /* contents intact, no overlaps (sorted sweep) */
    static int idx[SLOTS]; int k = 0;
    for (int i = 0; i < SLOTS; i++) if (s[i].p) {
        for (size_t j = 0; j < s[i].n; j++) if (s[i].p[j] != s[i].fill) return -1;
        idx[k++] = i;
    }
    for (int a = 1; a < k; a++) for (int b = a; b > 0 && s[idx[b-1]].p > s[idx[b]].p; b--) { int t = idx[b]; idx[b] = idx[b-1]; idx[b-1] = t; }
    for (int a = 1; a < k; a++) if (s[idx[a-1]].p + s[idx[a-1]].n > s[idx[a]].p) return -2;
    return 0;
}
int main(void) {
    unsigned char *region = malloc(REGION + 64);
    Yw4Heap *h = yw4_heap_create(region + 3, REGION);  /* unaligned on purpose */
    if (!h) FAIL("create failed");
    size_t empty = yw4_heap_total_free(h);
    if (yw4_heap_largest_free(h) != empty) FAIL("fresh heap: largest %zu != free %zu", yw4_heap_largest_free(h), empty);
    for (int step = 0; step < 300000; step++) {
        int i = rnd() % SLOTS;
        unsigned op = rnd() % 8;
        size_t n = (rnd() % 4 == 0) ? rnd() % 200000 : rnd() % 300;
        if (!s[i].p && op < 6) {
            size_t align = (op == 5) ? (size_t)1 << (rnd() % 13) : 16;
            unsigned char *q = yw4_heap_alloc(h, n, align);
            if (!q) continue;
            if ((size_t)q % (align < 16 ? 16 : align)) FAIL("misaligned %p align %zu", (void *)q, align);
            s[i].p = q; s[i].n = n; s[i].fill = (unsigned char)rnd();
            memset(q, s[i].fill, n);
        } else if (s[i].p && op < 4) {
            if (yw4_heap_free(h, s[i].p)) FAIL("free of live block refused");
            if (yw4_heap_free(h, s[i].p) == 0) FAIL("double free accepted");
            s[i].p = NULL;
        } else if (s[i].p) {
            unsigned char *q = yw4_heap_realloc(h, s[i].p, n);
            if (!q) continue;
            for (size_t j = 0; j < (n < s[i].n ? n : s[i].n); j++) if (q[j] != s[i].fill) FAIL("realloc lost data");
            s[i].p = q; s[i].n = n; memset(q, s[i].fill, n);
        }
        if (step % 20000 == 0) {
            int r = check_live();
            if (r) FAIL("step %d: %s", step, r == -1 ? "contents corrupted" : "blocks overlap");
            if (yw4_heap_largest_free(h) > yw4_heap_total_free(h)) FAIL("largest > total free");
        }
    }
    if (check_live()) FAIL("final check failed");
    if (yw4_heap_free(h, region + 4096 + 16) == 0) FAIL("stray pointer accepted");
    for (int i = 0; i < SLOTS; i++) if (s[i].p && yw4_heap_free(h, s[i].p)) FAIL("final free refused");
    if (yw4_heap_total_free(h) != empty || yw4_heap_largest_free(h) != empty)
        FAIL("not fully merged: free %zu largest %zu, expected %zu", yw4_heap_total_free(h), yw4_heap_largest_free(h), empty);
    if (yw4_heap_alloc(h, empty, 16) == NULL) FAIL("whole heap not allocatable");
    if (yw4_heap_alloc(h, 1, 16) != NULL) FAIL("allocation from a full heap");
    puts("ok");
    return 0;
}
"""


class HeapTest(unittest.TestCase):
    """The allocator behind nn::mem::StandardAllocator (runtime/src/heap.c, decision 0011)."""

    def test_random_operations(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "drv.c").write_text(HEAP_DRIVER)
            exe = t / "heap.exe"
            subprocess.run([check.host_cc(), "-std=c11", "-O1", "-Wall", f"-I{RUNTIME / 'include'}",
                            f"-I{RUNTIME / 'src'}", "-o", str(exe), str(t / "drv.c"), str(RUNTIME / "src" / "heap.c")],
                           check=True, capture_output=True)
            got = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(got.stdout.strip(), "ok")


STRING_DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "hle.h"
static CPUState c;
static long long call(GuestFn f, uint64_t a, uint64_t b, uint64_t n) {
    c.x[0] = a; c.x[1] = b; c.x[2] = n; f(&c); return (long long)c.x[0];
}
#define S(p) ((uint64_t)(uintptr_t)(p))
#define EXPECT(e, want) do { long long g_ = (e); if (g_ != (want)) { printf("%s = %lld, want %lld\n", #e, g_, (long long)(want)); return 1; } } while (0)
int main(void) {
    /* musl returns the byte difference, as unsigned chars */
    EXPECT(call(hle_libc_strcmp, S("abc"), S("abd"), 0), 'c' - 'd');
    EXPECT(call(hle_libc_strcmp, S("\xff"), S("a"), 0), 0xff - 'a');
    EXPECT(call(hle_libc_strcmp, S("ab"), S("abc"), 0), -'c');
    EXPECT(call(hle_libc_memcmp, S("a\x80"), S("a\x01"), 2), 0x7f);
    EXPECT(call(hle_libc_memcmp, S("xyz"), S("abc"), 0), 0);
    EXPECT(call(hle_libc_strncmp, S("abcx"), S("abcy"), 3), 0);
    EXPECT(call(hle_libc_strncmp, S("abcx"), S("abcy"), 4), 'x' - 'y');
    EXPECT(call(hle_libc_strncmp, S("a"), S("b"), 0), 0);
    EXPECT(call(hle_libc_strcasecmp, S("HeLLo"), S("hello"), 0), 0);
    EXPECT(call(hle_libc_strcasecmp, S("A"), S("b"), 0), 'a' - 'b');
    /* "C" locale ctype: nothing above 127, EOF in no class */
    EXPECT(call(hle_libc_isspace, 0xa0, 0, 0), 0);
    EXPECT(call(hle_libc_isspace, '\v', 0, 0), 1);
    EXPECT(call(hle_libc_isalnum, '@', 0, 0), 0);
    EXPECT(call(hle_libc_isalnum, 'Z', 0, 0), 1);
    EXPECT(call(hle_libc_ispunct, '~', 0, 0), 1);
    EXPECT(call(hle_libc_ispunct, ' ', 0, 0), 0);
    EXPECT(call(hle_libc_isxdigit, 'g', 0, 0), 0);
    EXPECT(call(hle_libc_isdigit, (uint32_t)-1, 0, 0), 0);
    EXPECT(call(hle_libc_toupper, 0xe9, 0, 0), 0xe9);
    EXPECT(call(hle_libc_tolower, 'Q', 0, 0), 'q');
    /* integers: guest long is 64-bit; atoi wraps like musl */
    EXPECT(call(hle_libc_atoi, S("  -42z"), 0, 0), -42);
    EXPECT(call(hle_libc_atoi, S("2147483648"), 0, 0), -2147483647 - 1);
    EXPECT(call(hle_libc_abs, (uint32_t)-7, 0, 0), 7);
    char *end = 0;
    EXPECT(call(hle_libc_strtol, S("0x7fffffffff!"), S(&end), 0), 0x7fffffffffLL);
    EXPECT(*end, '!');
    EXPECT(call(hle_libc_strtoul, S("-1"), 0, 10), -1);
    EXPECT(call(hle_libc_strtol, S("99999999999999999999"), 0, 10), 0x7fffffffffffffffLL);
    call(hle_libc_errno_location, 0, 0, 0);
    EXPECT(*(int *)(uintptr_t)c.x[0], 34);
    char buf[] = "a,,b;c";
    EXPECT(call(hle_libc_strtok, S(buf), S(",;"), 0), S(buf));
    EXPECT(call(hle_libc_strtok, 0, S(",;"), 0), S(buf + 3));
    EXPECT(call(hle_libc_strtok, 0, S(",;"), 0), S(buf + 5));
    EXPECT(call(hle_libc_strtok, 0, S(",;"), 0), 0);
    EXPECT(call(hle_libc_strnlen, S("abcdef"), 4, 0), 4);
    unsigned char m[8] = {0};
    EXPECT(call(hle_libc_memset_s, S(m), 4, 0x55) , 0);  /* x3 = 0 here: n = 0 */
    c.x[0] = S(m); c.x[1] = 4; c.x[2] = 0x55; c.x[3] = 8; hle_libc_memset_s(&c);
    EXPECT((long long)c.x[0], 22);
    EXPECT(m[3] == 0x55 && m[4] == 0, 1);
    puts("ok");
    return 0;
}
"""


class StringTest(unittest.TestCase):
    """C library functions where musl's exact result is reproduced (hle_string.c, decision 0012)."""

    def test_musl_conventions(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "drv.c").write_text(STRING_DRIVER)
            exe = t / "str.exe"
            subprocess.run([check.host_cc(), "-std=c11", "-O1", "-Wall", f"-I{RUNTIME / 'include'}",
                            f"-I{RUNTIME / 'src'}", "-o", str(exe), str(t / "drv.c"),
                            str(RUNTIME / "src" / "hle_string.c")], check=True, capture_output=True)
            got = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(got.stdout.strip(), "ok")


if __name__ == "__main__":
    unittest.main()


ROMFS_DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include "romfs.h"
int main(int argc, char **argv) {
    if (romfs_open(argv[1])) return 1;
    uint64_t n = romfs_size();
    unsigned char *img = malloc(n);
    /* read in odd-sized pieces, so reads cross file and padding boundaries */
    for (uint64_t at = 0; at < n;) {
        uint64_t k = n - at < 37 ? n - at : 37;
        if (romfs_read(at, img + at, k)) return 2;
        at += k;
    }
    FILE *out = fopen(argv[2], "wb");  /* not stdout: Windows text mode would change the bytes */
    if (!out || fwrite(img, 1, n, out) != n) return 3;
    fclose(out);
    return 0;
}
"""


def romfs_lookup(img: bytes, path: str) -> bytes | None:
    """Finds a file the way the game does: through the RomFS hash tables (independent of romfs.c)."""
    import struct as st
    hv = st.unpack_from("<10Q", img, 0)
    dhash, dhash_size, dmeta, _, fhash, fhash_size, fmeta, _, data = hv[1], hv[2], hv[3], hv[4], hv[5], hv[6], hv[7], hv[8], hv[9]

    def h(parent: int, name: bytes) -> int:
        x = parent ^ 123456789
        for c in name:
            x = (((x >> 5) | (x << 27)) & 0xFFFFFFFF) ^ c
        return x

    def find(table, nbuckets, meta, parent, name, entry_size):
        e = st.unpack_from("<I", img, table + 4 * (h(parent, name) % nbuckets))[0]
        while e != 0xFFFFFFFF:
            off = meta + e
            p = st.unpack_from("<I", img, off)[0]
            next_ = st.unpack_from("<I", img, off + entry_size - 8)[0]
            nl = st.unpack_from("<I", img, off + entry_size - 4)[0]
            if p == parent and img[off + entry_size:off + entry_size + nl] == name:
                return e, off
            e = next_
        return None

    parts = path.encode().split(b"/")
    parent = 0
    for d in parts[:-1]:
        r = find(dhash, dhash_size // 4, dmeta, parent, d, 0x18)
        if not r:
            return None
        parent = r[0]
    r = find(fhash, fhash_size // 4, fmeta, parent, parts[-1], 0x20)
    if not r:
        return None
    doff, size = st.unpack_from("<QQ", img, r[1] + 8)
    return img[data + doff:data + doff + size]


class RomfsTest(unittest.TestCase):
    """The virtual RomFS image (runtime/src/romfs.c): what the game's RomFS code would find."""

    def test_files_found_through_hash_tables(self):
        rng = random.Random(7)
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            root = t / "romfs"
            want = {}
            for path, size in [("a.bin", 0), ("b.txt", 1), ("dir1/x", 17), ("dir1/sub/deep/file.dat", 100),
                               ("dir1/sub/y", 4096 + 3), ("dir2/abcdefghijklm.n", 33), ("z/1", 15), ("z/2", 16)]:
                p = root / path
                p.parent.mkdir(parents=True, exist_ok=True)
                data = bytes(rng.getrandbits(8) for _ in range(size))
                p.write_bytes(data)
                want[path] = data
            (root / "emptydir").mkdir()
            (t / "drv.c").write_text(ROMFS_DRIVER)
            exe = t / "romfs.exe"
            # host.c: romfs.c's own lock around file reads and yw4_exit (every exit names its source line)
            subprocess.run([check.host_cc(), "-std=c11", "-O1", "-Wall", *POSIX, f"-I{RUNTIME / 'src'}", "-o", str(exe),
                            str(t / "drv.c"), str(RUNTIME / "src" / "romfs.c"), str(RUNTIME / "src" / "modmerge.c"), str(RUNTIME / "src" / "host.c"),
                            "-pthread", *(["-lwinmm"] if sys.platform == "win32" else [])],
                           check=True, capture_output=True)
            subprocess.run([str(exe), str(root), str(t / "image.bin")], capture_output=True, check=True)
            img = (t / "image.bin").read_bytes()
            self.assertEqual(int.from_bytes(img[:8], "little"), 0x50)
            for path, data in want.items():
                self.assertEqual(romfs_lookup(img, path), data, path)
            self.assertIsNone(romfs_lookup(img, "dir1/missing"))
            self.assertIsNone(romfs_lookup(img, "nodir/x"))

    def test_mods_cpk_lists_merged(self):
        """Two mods each free their own files in a copy of data/cpk_list.cfg.bin: the game sees both sets freed
        (decision 0067). One mod uses the data/-only layout; one file is listed in two archives."""
        import struct as st

        def cfg(entries):  # Level-5 cfg.bin: (path, archive or None for 0xFFFFFFFF or "" for an empty string, size)
            strs, offs = bytearray(), {}

            def s(x):
                if x not in offs:
                    offs[x] = len(strs)
                    strs.extend(x.encode() + b"\0")
                return offs[x]
            body = bytearray()
            for path, arch, size in entries:
                body += st.pack("<IB", 0x36F3E0D3, 3) + bytes([0b010000]) + b"\0\0"  # types: string, string, int
                body += st.pack("<III", s(path), 0xFFFFFFFF if arch is None else s(arch), size)
            st_off = 0x10 + len(body)
            return st.pack("<4I", len(entries), st_off, len(strs), len(offs)) + body + bytes(strs)

        def archives(b):
            n, st_off, _, _ = st.unpack_from("<4I", b)
            out, p = [], 0x10
            for _ in range(n):
                path, arch, _ = st.unpack_from("<III", b, p + 8)
                name = lambda o: b[st_off + o:b.index(b"\0", st_off + o)].decode()
                out.append((name(path), None if arch == 0xFFFFFFFF else name(arch) or None))
                p += 20
            return out

        game = [("data/a", "x.cpk", 4), ("data/b", "x.cpk", 4), ("data/b", "y.cpk", 4), ("data/c", "x.cpk", 4),
                ("data/d", "x.cpk", 4)]
        free = lambda names: [(p, "" if p in names else a, n) for p, a, n in game]
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            files = {"romfs/data/cpk_list.cfg.bin": cfg(game),
                     "mods/A mod/romfs/data/cpk_list.cfg.bin": cfg(free({"data/a", "data/b"})),
                     "mods/A mod/romfs/data/a": b"modA-longer", "mods/A mod/romfs/data/b": b"modB",
                     "mods/b_mod/data/cpk_list.cfg.bin": cfg(free({"data/c"})), "mods/b_mod/data/c": b"modC"}
            for path, data in files.items():
                (t / path).parent.mkdir(parents=True, exist_ok=True)
                (t / path).write_bytes(data)
            (t / "drv.c").write_text(ROMFS_DRIVER)
            exe = t / "romfs.exe"
            subprocess.run([check.host_cc(), "-std=c11", "-O1", "-Wall", *POSIX, f"-I{RUNTIME / 'src'}", "-o", str(exe),
                            str(t / "drv.c"), str(RUNTIME / "src" / "romfs.c"), str(RUNTIME / "src" / "modmerge.c"), str(RUNTIME / "src" / "host.c"),
                            "-pthread", *(["-lwinmm"] if sys.platform == "win32" else [])],
                           check=True, capture_output=True)
            r = subprocess.run([str(exe), str(t / "romfs"), str(t / "image.bin")], capture_output=True, text=True,
                               env={**os.environ, "YW4_MODS": str(t / "mods")})
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("b_mod: 1 files replaced, 1 added", r.stderr)  # the data/-only layout is found
            self.assertIn("2 mods' lists merged, 3 entries freed", r.stderr)
            # a loose file's listed size is the file read (A's data/a is 11 bytes, every list says 4): the game reads a
            # loose file by its entry's size (2026-11-14: a wrong one blanked the English patch's item names)
            self.assertIn("cpk_list: 1 loose files' sizes set to the files read", r.stderr)
            img = (t / "image.bin").read_bytes()
            self.assertEqual(romfs_lookup(img, "data/c"), b"modC")
            lst = romfs_lookup(img, "data/cpk_list.cfg.bin")
            sizes = {name: st.unpack_from("<I", lst, 16 + 20 * i + 16)[0]
                     for i, (name, _) in enumerate(archives(lst))}
            self.assertEqual(sizes, {"data/a": 11, "data/b": 4, "data/c": 4, "data/d": 4})
            self.assertEqual(archives(romfs_lookup(img, "data/cpk_list.cfg.bin")),
                             [("data/a", None), ("data/b", None), ("data/b", None), ("data/c", None),
                              ("data/d", "x.cpk")])

    def test_mods_data_files_merged(self):
        """Mods changing different entries of one .cfg.bin all keep their changes; two changing the same entry
        clash, the later mod's copy is used, and `--mod-clashes` names them (decision 0068)."""
        import struct as st

        def cfg(vals):  # entries of one int each: (crc, value)
            body = b"".join(st.pack("<IB", crc, 1) + bytes([0b01]) + b"\xff\xff" + st.pack("<I", v) for crc, v in vals)
            return st.pack("<4I", len(vals), 16 + len(body), 0, 0) + body

        def entries(b):
            n = st.unpack_from("<I", b)[0]
            return [st.unpack_from("<I", b, 16 + 12 * i)[0:1] + st.unpack_from("<I", b, 16 + 12 * i + 8) for i in range(n)]

        base = [(i, 100 + i) for i in range(1, 9)]
        items_a = [(c, 999 if c == 2 else v) for c, v in base]                       # A changes entry 2
        items_b = [(c, 555 if c == 6 else v) for c, v in base[:7]] + [(77, 7)] + base[7:]  # B: entry 6, adds one
        shop_a = [(c, 1 if c == 3 else v) for c, v in base]                          # A and C: both entry 3
        shop_c = [(c, 2 if c == 3 else v) for c, v in base]
        drv_clash = '#include <stdio.h>\n#include "romfs.h"\nint main(int c, char **v) { return romfs_mod_clashes(v[1], stdout); }\n'
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            files = {"romfs/data/items.cfg.bin": cfg(base), "romfs/data/shop.cfg.bin": cfg(base),
                     "romfs/data/pic.bin": b"game",
                     "mods/A/romfs/data/items.cfg.bin": cfg(items_a), "mods/A/romfs/data/shop.cfg.bin": cfg(shop_a),
                     "mods/A/romfs/data/pic.bin": b"A",
                     "mods/B/romfs/data/items.cfg.bin": cfg(items_b),
                     "mods/C/romfs/data/shop.cfg.bin": cfg(shop_c), "mods/C/romfs/data/pic.bin": b"C"}
            for path, data in files.items():
                (t / path).parent.mkdir(parents=True, exist_ok=True)
                (t / path).write_bytes(data)
            (t / "drv.c").write_text(ROMFS_DRIVER)
            (t / "clash.c").write_text(drv_clash)
            srcs = [str(RUNTIME / "src" / n) for n in ("romfs.c", "modmerge.c", "host.c")]
            libs = ["-pthread", *(["-lwinmm"] if sys.platform == "win32" else [])]
            for drv, exe in (("drv.c", "romfs.exe"), ("clash.c", "clash.exe")):
                subprocess.run([check.host_cc(), "-std=c11", "-O1", "-Wall", *POSIX, f"-I{RUNTIME / 'src'}", "-o",
                                str(t / exe), str(t / drv), *srcs, *libs], check=True, capture_output=True)
            env = {**os.environ, "YW4_MODS": str(t / "mods")}
            r = subprocess.run([str(t / "romfs.exe"), str(t / "romfs"), str(t / "image.bin")], capture_output=True,
                               text=True, env=env)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("data/items.cfg.bin: merged from 2 mods", r.stderr)
            self.assertIn("data/shop.cfg.bin: A and C change the same entries, C's copy is used", r.stderr)
            img = (t / "image.bin").read_bytes()
            want = [(c, 999 if c == 2 else 555 if c == 6 else v) for c, v in base[:7]] + [(77, 7)] + base[7:]
            self.assertEqual(entries(romfs_lookup(img, "data/items.cfg.bin")), want)
            self.assertEqual(entries(romfs_lookup(img, "data/shop.cfg.bin")), shop_c)
            r = subprocess.run([str(t / "clash.exe"), str(t / "romfs")], capture_output=True, text=True, env=env)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(sorted(r.stdout.splitlines()),
                             ["clash\tdata/pic.bin\tA\tC", "clash\tdata/shop.cfg.bin\tA\tC"])


SWKBD_DRIVER = r"""
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "yw4cpu.h"

typedef void (*GuestFn)(CPUState *ctx);

static int prompt_step = 0;
static char last_message_title[128];
static char last_message_body[128];
static int message_shown = 0;

void gamemenu_prompt_options(int min_chars, int cancel) {}
void gamemenu_prompt_begin(const char *title, int max_chars, const char *fallback) {}
int gamemenu_prompt_take(char *out, size_t n) {
    if (message_shown) {
        message_shown = 0;
        return 1; /* message dismissed */
    }
    if (prompt_step == 0) {
        prompt_step++;
        if (out) snprintf(out, n, "BadName");
        return 1;
    }
    if (prompt_step == 1) {
        prompt_step++;
        if (out) snprintf(out, n, "GoodName");
        return 1;
    }
    return 0;
}
void gamemenu_message_begin(const char *title, const char *body) {
    message_shown = 1;
    strncpy(last_message_title, title, sizeof(last_message_title) - 1);
    strncpy(last_message_body, body, sizeof(last_message_body) - 1);
}
int window_repaint(int force) { return 0; }
void host_sleep_ns(int64_t ns) {}
/* the main characters' default names (main_name_for): a translation mod's chara_text, or none */
static const char *mod_chara_text;
const char *romfs_host_path(const char *path) {
    return mod_chara_text && !strcmp(path, "data/common/text/ja/chara_text.cfg.bin") ? mod_chara_text : NULL;
}

static uint64_t last_user_arg;
static char last_checked_text[128];
static uint64_t last_string_size;

static void mock_callback(CPUState *ctx) {
    last_user_arg = ctx->x[0];
    void *str_ptr = *(void **)ctx->x[2];
    last_string_size = *(uint64_t *)((uintptr_t)ctx->x[2] + 8);
    strncpy(last_checked_text, (const char *)str_ptr, sizeof(last_checked_text) - 1);
    if (strcmp(last_checked_text, "BadName") == 0) {
        ctx->x[0] = 1; /* rejected */
    } else {
        ctx->x[0] = 0; /* accepted */
    }
}

GuestFn yw4_resolve(uint64_t target) {
    if (target == 0x12345678) return mock_callback;
    return NULL;
}

#include "swkbd.c"

/* the loader's module table: the typed-name hook (decision 0066) reads main's base from it */
static uint64_t main_base;
const Yw4Module yw4_modules[] = { { .name = "main", .base = &main_base } };

/* main_name_for reads its names once per run, so each case is its own run: argv[1] "mod" (argv[2] a file laid
 * out as the English patch's: 0xFF filler, six names, Neptuna, the six tokens) or "none" (the Japanese names). */
static int names_test(int argc, char **argv) {
    if (!strcmp(argv[1], "mod")) {
        if (argc < 3) return 30;
        mod_chara_text = argv[2];
        if (strcmp(main_name_for("<NAME_KETA_8888>") ?: "", "Nate")) return 31;
        if (strcmp(main_name_for("<NAME_NATSUME_8>") ?: "", "Summer")) return 32;
        if (strcmp(main_name_for("<NAME_TOUMA_888>") ?: "", "Touma")) return 33;  /* the first, after the filler */
        if (strcmp(main_name_for("<NAME_FUMI_8888>") ?: "", "Katie")) return 34;
    } else {
        if (strcmp(main_name_for("<NAME_KETA_8888>") ?: "", "ケータ")) return 35;
        if (strcmp(main_name_for("<NAME_FUMI_8888>") ?: "", "フミ")) return 36;
    }
    if (main_name_for("<ITEM_NAME>")) return 37;  /* not a main character's token */
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) return names_test(argc, argv);
    CPUState c = {0};
    uint8_t a[0x500] = {0};
    char buf[256] = {0};
    uint64_t out_struct[2] = { (uint64_t)(uintptr_t)buf, sizeof(buf) };

    c.x[0] = (uint64_t)(uintptr_t)out_struct;
    c.x[1] = (uint64_t)(uintptr_t)a;

    /* 1. With textcheck disabled */
    a[0x3d0] = 0;
    *(uint64_t *)(a + 0x3d8) = 0x12345678;
    if (check_name(&c, a, "BadName", 1, 8) != 0) return 1;

    /* 2. With textcheck enabled, valid name */
    a[0x3d0] = 1;
    *(uint64_t *)(a + 0x4e0) = 0xbeefcafe;
    a[0x3bd] = 1;
    if (check_name(&c, a, "GoodName", 1, 8) != 0) return 2;
    if (last_user_arg != 0xbeefcafe) return 3;
    if (strcmp(last_checked_text, "GoodName") != 0) return 4;
    if (last_string_size != 8) return 5;
    if (out_struct[1] != sizeof(buf)) return 6;

    /* 3. With textcheck enabled, rejected name */
    if (check_name(&c, a, "BadName", 1, 8) != 1) return 7;
    if (strcmp(last_checked_text, "BadName") != 0) return 8;
    if (out_struct[1] != sizeof(buf)) return 9;

    /* 4. Full interactive ShowKeyboard flow with initial rejection and retry */
    prompt_step = 0;
    memset(buf, 0, sizeof(buf));
    *(int32_t *)(a + 0x3ac) = 8; /* max */
    yw4_swkbd_ShowKeyboard(&c);
    if (c.x[0] != 0) return 10; /* ShowKeyboard should succeed after retry */
    if (strcmp(buf, "GoodName") != 0) return 11;
    if (strcmp(last_message_body, "This name cannot be used.") != 0) return 12;

    /* 5. Profanity filter native overrides */
    CPUState pf_ctx = {0};
    yw4_ngc_ProfanityFilter_Initialize(&pf_ctx);
    if (pf_ctx.x[0] != 0) return 13;
    yw4_ngc_ProfanityFilter_GetContentVersion(&pf_ctx);
    if (pf_ctx.x[0] != 1) return 14;
    int words_masked = -1;
    pf_ctx.x[1] = (uint64_t)(uintptr_t)&words_masked;
    yw4_ngc_ProfanityFilter_MaskProfanityWordsInText_c(&pf_ctx);
    if (pf_ctx.x[0] != 0 || words_masked != 0) return 15;
    uint32_t check_res[2] = {99, 99};
    pf_ctx.x[1] = (uint64_t)(uintptr_t)check_res;
    pf_ctx.x[4] = 2;
    yw4_ngc_ProfanityFilter_CheckProfanityWords_c(&pf_ctx);
    if (pf_ctx.x[0] != 0 || check_res[0] != 0 || check_res[1] != 0) return 16;
    yw4_ngc_ProfanityFilter_Finalize(&pf_ctx);
    if (pf_ctx.x[0] != 0) return 17;

    return 0;
}
"""


class SwkbdCheckTest(unittest.TestCase):
    """The software keyboard text check callback (runtime/src/swkbd.c)."""

    def test_text_check_callback(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "drv.c").write_text(SWKBD_DRIVER, encoding="utf-8")
            exe = t / "swkbd_test.exe"
            subprocess.run([check.host_cc(), "-std=gnu17", "-O1", "-Wall",
                            f"-I{RUNTIME / 'include'}", f"-I{RUNTIME / 'src'}",
                            f"-I{check.REPO / 'third_party' / 'vulkan' / 'include'}",
                            *POSIX,
                            "-o", str(exe), str(t / "drv.c")], check=True, capture_output=True)
            got = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
            self.assertEqual(got.returncode, 0)
            # the default names: a translation mod's file (the English patch's layout, made up here), then none
            names = [b"Touma", b"Summer", b"Akinori", b"Shin", b"Nate", b"Katie", b"Neptuna"]
            tokens = [b"<NAME_TOUMA_888>", b"<NAME_NATSUME_8>", b"<NAME_AKINORI_8>", b"<NAME_SHIN_8888>",
                      b"<NAME_KETA_8888>", b"<NAME_FUMI_8888>"]
            chara = t / "chara_text.cfg.bin"
            chara.write_bytes(b"\x00" * 16 + b"\xff\xff\xff" + b"\x00".join(names + tokens) + b"\x00Dad\x00")
            for args in (["mod", str(chara)], ["none"]):
                got = subprocess.run([str(exe), *args], capture_output=True, text=True)
                self.assertEqual(got.returncode, 0, f"{args[0]}: {got.stderr}")


HID_VIBRATION_DRIVER = r"""
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "ipc.h"
#include "hle.h"
#include "host.h"
#include "kernel.h"

/* what svc_hid.c hands the window (decision 0058): each band's amplitude (0..1) and frequency */
static float last_low, last_freq_low, last_high, last_freq_high;
static int rumble_calls;

void window_vibration(float amp_low, float freq_low, float amp_high, float freq_high) {
    last_low = amp_low, last_freq_low = freq_low;
    last_high = amp_high, last_freq_high = freq_high;
    rumble_calls++;
}
static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

void window_input(uint64_t *buttons, int32_t sticks[4]) {}
void host_lock(HostLock *l) {}
void host_unlock(HostLock *l) {}
uint64_t window_input_reads(void) { return 0; }
void window_input_read_replayed(void) {}
int window_paused(void) { return 0; }
uint64_t window_pause_buttons(void) { return 0; }
void settings_action(int a) {}
int settings_open(void) { return 0; }
int gamemenu_open(void) { return 0; }
void textin_type(const char *t) {}
int bindings_capture_axis(int axis, int16_t val) { return 0; }
void used_pad(int p) {}
void panel_pad_stick(void) {}
void panel_pad_triggers(void) {}
KSharedMemory *yw4_shared_memory_new(size_t size, void (*on_map)(KSharedMemory *), uint32_t *handle) { return NULL; }
void yw4_handle_close(uint32_t handle) {}
KEvent *yw4_event_new(uint32_t *handle) { return NULL; }
void gpu_request_trace(int draws) {}
void gpu_request_dump(void) {}
void shadow_toggle(void) {}

Service *ipc_service_new(const char *name, ServiceHandler handler, void *state) { return NULL; }
void ipc_unimplemented(Service *s, IpcRequest *req, IpcResponse *resp) {}
void ipc_return_object(IpcResponse *resp, Service *s) {}

#include "svc_hid.c"

int main(void) {
    Service s = {"hid", svc_hid, NULL, 1};
    IpcResponse resp = {0};

    /* 1. Test SendVibrationValues (cmd 206) with stereo left/right actuators */
    uint32_t handles[2] = {
        0x00010001, /* style 1, npad 0 (player 1), device 1 (left) */
        0x00020001  /* style 1, npad 0 (player 1), device 2 (right) */
    };
    VibrationValue vals[2] = {
        { 0.5f, 160.0f, 0.2f, 320.0f },
        { 0.8f, 160.0f, 0.4f, 320.0f }
    };
    IpcRequest req206 = {0};
    req206.cmd = 206;
    req206.nx = 2;
    req206.x[0].addr = (uint64_t)(uintptr_t)handles;
    req206.x[0].size = sizeof(handles);
    req206.x[1].addr = (uint64_t)(uintptr_t)vals;
    req206.x[1].size = sizeof(vals);

    rumble_calls = 0;
    svc_hid(&s, &req206, &resp);

    /* the stronger amplitude of the two devices per band, the bands' frequencies as sent */
    if (rumble_calls != 1) return 1;
    if (!near(last_low, 0.8f)) return 2;
    if (!near(last_high, 0.4f)) return 3;
    if (!near(last_freq_low, 160.0f) || !near(last_freq_high, 320.0f)) return 4;

    /* 2. Test stopping vibration (zero amplitude) */
    vals[0].amp_low = 0.0f; vals[0].amp_high = 0.0f;
    vals[1].amp_low = 0.0f; vals[1].amp_high = 0.0f;
    svc_hid(&s, &req206, &resp);
    if (last_low != 0 || last_high != 0) return 5;

    /* 3. Test player 2 handle ignored */
    uint32_t p2_handle = 0x00010101; /* npad 1 (player 2) */
    VibrationValue p2_val = { 1.0f, 160.0f, 1.0f, 320.0f };
    req206.x[0].addr = (uint64_t)(uintptr_t)&p2_handle;
    req206.x[0].size = sizeof(p2_handle);
    req206.x[1].addr = (uint64_t)(uintptr_t)&p2_val;
    req206.x[1].size = sizeof(p2_val);
    svc_hid(&s, &req206, &resp);
    if (last_low != 0 || last_high != 0) return 6;

    /* 4. Test SendVibrationValue (cmd 201, singular) */
    uint8_t raw201[32] = {0};
    *(uint32_t *)(raw201 + 0) = 0x00010001; /* player 1, device 1 */
    VibrationValue *v201 = (VibrationValue *)(raw201 + 4);
    v201->amp_low = 0.6f;
    v201->amp_high = 0.3f;
    IpcRequest req201 = {0};
    req201.cmd = 201;
    req201.data = raw201;
    req201.data_size = 28;
    svc_hid(&s, &req201, &resp);
    if (!near(last_low, 0.6f)) return 7;
    if (!near(last_high, 0.3f)) return 8;

    return 0;
}
"""


class HidVibrationTest(unittest.TestCase):
    """The controller vibration handling (runtime/src/svc_hid.c)."""

    def test_vibration_routing(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "drv.c").write_text(HID_VIBRATION_DRIVER)
            exe = t / "vib_test.exe"
            subprocess.run([check.host_cc(), "-std=c17", "-O1", "-Wall",
                            f"-I{RUNTIME / 'include'}", f"-I{RUNTIME / 'src'}",
                            f"-I{check.REPO / 'third_party' / 'vulkan' / 'include'}",
                            *POSIX,
                            "-o", str(exe), str(t / "drv.c")], check=True, capture_output=True)
            got = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
            self.assertEqual(got.returncode, 0)

    def test_vibration_toggle(self):
        driver = r"""
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include <SDL3/SDL.h>
#include "yw4load.h"
#include "window.h"

int battle_active(void) { return 0; }
void settings_save(void) {}
void window_mouse_motion(float *dx, float *dy) { *dx = 0; *dy = 0; }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad) { return SDL_GAMEPAD_TYPE_STANDARD; }
const Yw4Module yw4_modules[2] = {0};

static int last_low, last_high, last_dur, rumble_called;
static int rumble_state = 1;

int window_rumble_enabled(void) { return rumble_state; }
void window_rumble_set_enabled(int on) { rumble_state = on; }
void window_rumble(uint16_t low, uint16_t high, uint32_t duration_ms) {
    last_low = low; last_high = high; last_dur = duration_ms; rumble_called++;
}
void window_rumble_test(void) { window_rumble(49152, 49152, 1000); }
#include "tegra.h"
/* the icon sheets' patch (bindings_patch_sheet) is not tested here: its GPU and texture-layout calls do nothing */
void gpu_note_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
void tegra_read_region(const TexLayout *t, const uint8_t *base, uint32_t level, uint32_t layer, uint32_t x, uint32_t y,
                       uint32_t z, uint32_t w, uint32_t h, uint32_t d, uint8_t *dst) {}
void tegra_write_region(const TexLayout *t, uint8_t *base, uint32_t level, uint32_t layer, uint32_t x, uint32_t y,
                        uint32_t z, uint32_t w, uint32_t h, uint32_t d, const uint8_t *src) {}
uint64_t tegra_total_size(const TexLayout *t) { return 0; }

#include "bindings.c"

int main(void) {
    BindRow rows[64];
    int r = 0, c = 0;
    char help[256];
    int n = bindings_rows(rows, 64, &r, &c, help, sizeof(help));
    if (n < 6) return 1;
    /* rows 0-1: the mouse's sensitivities; row 2: its buttons page (bed09f1); then the vibration toggle and
     * the Nintendo icons switch (04d8bba) */
    if (strcmp(rows[3].group, "CONTROLLER") != 0) return 2;
    if (strcmp(rows[3].name, "VIBRATION") != 0) return 3;
    if (strcmp(rows[3].cells[0], "ON") != 0) return 4;
    if (strcmp(rows[4].name, "NINTENDO ICONS") != 0) return 21;
    if (strcmp(rows[5].group, "KEYS FOR") != 0) return 20;

    /* select the vibration row and toggle off */
    bindings_select(3, 0);
    bindings_action(SETTINGS_ACCEPT);
    if (rumble_state != 0) return 5;

    /* toggle back on: should trigger 1-second rumble test */
    rumble_called = 0;
    bindings_action(SETTINGS_ACCEPT);
    if (rumble_state != 1) return 6;
    if (rumble_called != 1 || last_dur != 1000 || last_low != 49152 || last_high != 49152) return 7;

    /* test that SETTINGS_RIGHT loops from on to off to on */
    bindings_action(SETTINGS_RIGHT);
    if (rumble_state != 0) return 10;
    rumble_called = 0;
    bindings_action(SETTINGS_RIGHT);
    if (rumble_state != 1 || rumble_called != 1) return 11;

    /* test that SETTINGS_LEFT also loops */
    bindings_action(SETTINGS_LEFT);
    if (rumble_state != 0) return 12;
    rumble_called = 0;
    bindings_action(SETTINGS_LEFT);
    if (rumble_state != 1 || rumble_called != 1) return 13;

    /* load from ini line */
    bindings_load_line("YW4_RUMBLE", "0");
    if (rumble_state != 0) return 8;
    bindings_load_line("YW4_RUMBLE", "1");
    if (rumble_state != 1) return 9;

    return 0;
}
"""
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "drv_toggle.c").write_text(driver)
            exe = t / "toggle_test.exe"
            sdl = sdl3(t)
            if sdl is None:
                self.skipTest("no SDL3 on this system (pkg-config sdl3)")
            sdl_flags, sdl_libs = sdl
            subprocess.run([check.host_cc(), "-std=c17", "-O1", "-Wall",
                            f"-I{RUNTIME / 'include'}", f"-I{RUNTIME / 'src'}",
                            f"-I{check.REPO / 'third_party' / 'vulkan' / 'include'}",
                            *POSIX, *sdl_flags,
                            "-o", str(exe), str(t / "drv_toggle.c"), *sdl_libs], check=True, capture_output=True)
            got = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
            self.assertEqual(got.returncode, 0)


class BattleScrollTest(unittest.TestCase):
    """Mouse scroll in battle selects character (D-pad up/down) by default."""

    def test_battle_wheel_bindings(self):
        driver = r"""
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include <SDL3/SDL.h>
#include "yw4load.h"
#include "window.h"

static int battle_state;
int battle_active(void) { return battle_state; }
void settings_save(void) {}
void window_mouse_motion(float *dx, float *dy) { *dx = 0; *dy = 0; }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad) { return SDL_GAMEPAD_TYPE_STANDARD; }
const Yw4Module yw4_modules[2] = {0};

int window_rumble_enabled(void) { return 1; }
void window_rumble_set_enabled(int on) { (void)on; }
void window_rumble(uint16_t low, uint16_t high, uint32_t duration_ms) { (void)low; (void)high; (void)duration_ms; }
void window_rumble_test(void) {}
#include "tegra.h"
/* the icon sheets' patch (bindings_patch_sheet) is not tested here: its GPU and texture-layout calls do nothing */
void gpu_note_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
void tegra_read_region(const TexLayout *t, const uint8_t *base, uint32_t level, uint32_t layer, uint32_t x, uint32_t y,
                       uint32_t z, uint32_t w, uint32_t h, uint32_t d, uint8_t *dst) {}
void tegra_write_region(const TexLayout *t, uint8_t *base, uint32_t level, uint32_t layer, uint32_t x, uint32_t y,
                        uint32_t z, uint32_t w, uint32_t h, uint32_t d, const uint8_t *src) {}
uint64_t tegra_total_size(const TexLayout *t) { return 0; }

#include "bindings.c"

int main(void) {
    uint64_t b = 0;
    int32_t s[4] = {0};

    /* 1. Exploring: wheel up/down does NOT press D-pad up (bit 13) or down (bit 15) */
    battle_state = 0;
    bindings_read(NULL, 0, 1, 0, NULL, &b, s);
    if (b & (1ull << 13)) return 1;
    b = 0;
    bindings_read(NULL, 0, 0, 1, NULL, &b, s);
    if (b & (1ull << 15)) return 2;

    /* 2. In battle: wheel up presses D-pad up (bit 13) */
    battle_state = 1;
    b = 0;
    bindings_read(NULL, 0, 1, 0, NULL, &b, s);
    if (!(b & (1ull << 13))) return 3;
    if (b & (1ull << 15)) return 4;

    /* 3. In battle: wheel down presses D-pad down (bit 15) */
    b = 0;
    bindings_read(NULL, 0, 0, 1, NULL, &b, s);
    if (!(b & (1ull << 15))) return 5;
    if (b & (1ull << 13)) return 6;

    /* 4. Upgrade older settings lines where KEY2 was '-' */
    bindings_load_line("BATTLE_KEY1_DPAD_UP", "Up");
    bindings_load_line("BATTLE_KEY2_DPAD_UP", "-");
    bindings_load_line("BATTLE_KEY1_DPAD_DOWN", "Down");
    bindings_load_line("BATTLE_KEY2_DPAD_DOWN", "-");

    b = 0;
    bindings_read(NULL, 0, 1, 0, NULL, &b, s);
    if (!(b & (1ull << 13))) return 7;

    b = 0;
    bindings_read(NULL, 0, 0, 1, NULL, &b, s);
    if (!(b & (1ull << 15))) return 8;

    /* 5. Both Arrow Keys and D-pad remain fully active default options */
    bool kbd[SDL_SCANCODE_COUNT] = {0};
    battle_state = 1;
    kbd[SDL_SCANCODE_UP] = true;
    b = 0;
    bindings_read(kbd, 0, 0, 0, NULL, &b, s);
    if (!(b & (1ull << 13))) return 9;
    kbd[SDL_SCANCODE_UP] = false;

    kbd[SDL_SCANCODE_DOWN] = true;
    b = 0;
    bindings_read(kbd, 0, 0, 0, NULL, &b, s);
    if (!(b & (1ull << 15))) return 10;
    kbd[SDL_SCANCODE_DOWN] = false;

    battle_state = 0;
    kbd[SDL_SCANCODE_UP] = true;
    b = 0;
    bindings_read(kbd, 0, 0, 0, NULL, &b, s);
    if (!(b & (1ull << 13))) return 11;
    kbd[SDL_SCANCODE_UP] = false;

    kbd[SDL_SCANCODE_DOWN] = true;
    b = 0;
    bindings_read(kbd, 0, 0, 0, NULL, &b, s);
    if (!(b & (1ull << 15))) return 12;
    kbd[SDL_SCANCODE_DOWN] = false;

    return 0;
}
"""
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "drv_battle_scroll.c").write_text(driver)
            exe = t / "battle_scroll_test.exe"
            sdl = sdl3(t)
            if sdl is None:
                self.skipTest("no SDL3 on this system (pkg-config sdl3)")
            sdl_flags, sdl_libs = sdl
            subprocess.run([check.host_cc(), "-std=c17", "-O1", "-Wall",
                            f"-I{RUNTIME / 'include'}", f"-I{RUNTIME / 'src'}",
                            f"-I{check.REPO / 'third_party' / 'vulkan' / 'include'}",
                            *POSIX, *sdl_flags,
                            "-o", str(exe), str(t / "drv_battle_scroll.c"), *sdl_libs], check=True, capture_output=True)
            got = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
            self.assertEqual(got.returncode, 0)
