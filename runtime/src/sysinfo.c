/* The machine the run is on, printed once at the start of every log: OS, CPU, RAM, the disk the game's folder is on, and
 * the display session. A bug report then says what the player has (issue #14, 2026-11-16: a crash on an Intel HD 520,
 * and the log could not say how much RAM the machine had or how full its disk was). Read once, at start; nothing here
 * is sent anywhere. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#define GIB (1024.0 * 1024.0 * 1024.0)

#ifndef _WIN32
/* The text after `key` on the first line of `file` that starts with it (spaces, ':', '=' and quotes skipped). */
static int file_value(const char *file, const char *key, char *out, size_t n) {
    FILE *f = fopen(file, "r");
    if (!f) return 0;
    char line[512];
    int ok = 0;
    size_t klen = strlen(key);
    while (!ok && fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) != 0) continue;
        char *v = line + klen;
        while (*v == ' ' || *v == '\t' || *v == ':' || *v == '=' || *v == '"') v++;
        v[strcspn(v, "\r\n")] = 0;
        size_t len = strlen(v);
        while (len && v[len - 1] == '"') v[--len] = 0;
        snprintf(out, n, "%s", v);
        ok = 1;
    }
    fclose(f);
    return ok;
}

/* A /proc/meminfo figure ("MemTotal:   16384 kB") in bytes; 0 when it is not there. */
static uint64_t meminfo_bytes(const char *key) {
    char v[64];
    if (!file_value("/proc/meminfo", key, v, sizeof v)) return 0;
    return strtoull(v, NULL, 10) * 1024;
}
#endif

void yw4_sysinfo_dump(void) {
#ifdef _WIN32
    OSVERSIONINFOW vi = {.dwOSVersionInfoSize = sizeof vi};
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    RtlGetVersionFn rtl = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    if (rtl && rtl(&vi) == 0)
        fprintf(stderr, "yw4: system: OS Windows %lu.%lu build %lu\n", (unsigned long)vi.dwMajorVersion,
                (unsigned long)vi.dwMinorVersion, (unsigned long)vi.dwBuildNumber);
    else
        fprintf(stderr, "yw4: system: OS Windows (version not read)\n");
    /* 65001: UTF-8, from yw4.manifest; anything else: paths with non-ASCII characters may not be found */
    fprintf(stderr, "yw4: system: code page %u\n", GetACP());

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    char cpu[128] = "unknown";
    const char *id = getenv("PROCESSOR_IDENTIFIER");
    if (id) snprintf(cpu, sizeof cpu, "%s", id);
    fprintf(stderr, "yw4: system: CPU %s, %lu logical processors\n", cpu, (unsigned long)si.dwNumberOfProcessors);

    MEMORYSTATUSEX m = {.dwLength = sizeof m};
    if (GlobalMemoryStatusEx(&m))
        fprintf(stderr, "yw4: system: RAM %.1f GiB total, %.1f GiB available; page file %.1f GiB total, %.1f GiB available\n",
                (double)m.ullTotalPhys / GIB, (double)m.ullAvailPhys / GIB, (double)m.ullTotalPageFile / GIB,
                (double)m.ullAvailPageFile / GIB);

    ULARGE_INTEGER avail, total;
    if (GetDiskFreeSpaceExA(".", &avail, &total, NULL))
        fprintf(stderr, "yw4: system: disk of the game's folder %.1f GiB free of %.1f GiB\n", (double)avail.QuadPart / GIB,
                (double)total.QuadPart / GIB);
#else
    struct utsname u;
    if (uname(&u) == 0)
        fprintf(stderr, "yw4: system: kernel %s %s\n", u.release, u.machine);
    char os[160] = "unknown";
    file_value("/etc/os-release", "PRETTY_NAME", os, sizeof os);
    fprintf(stderr, "yw4: system: OS %s\n", os);

    char cpu[160] = "unknown";
    file_value("/proc/cpuinfo", "model name", cpu, sizeof cpu);
    long threads = sysconf(_SC_NPROCESSORS_ONLN);
    fprintf(stderr, "yw4: system: CPU %s, %ld logical processors\n", cpu, threads);

    uint64_t total = meminfo_bytes("MemTotal"), avail = meminfo_bytes("MemAvailable");
    uint64_t swap = meminfo_bytes("SwapTotal"), swapfree = meminfo_bytes("SwapFree");
    if (total)
        fprintf(stderr, "yw4: system: RAM %.1f GiB total, %.1f GiB available; swap %.1f GiB total, %.1f GiB free\n",
                (double)total / GIB, (double)avail / GIB, (double)swap / GIB, (double)swapfree / GIB);

    struct statvfs sv;
    if (statvfs(".", &sv) == 0)
        fprintf(stderr, "yw4: system: disk of the game's folder %.1f GiB free of %.1f GiB\n",
                (double)sv.f_bavail * (double)sv.f_frsize / GIB, (double)sv.f_blocks * (double)sv.f_frsize / GIB);

    const char *session = getenv("XDG_SESSION_TYPE");
    fprintf(stderr, "yw4: system: display session %s%s\n", session ? session : "unknown",
            getenv("WAYLAND_DISPLAY") ? " (Wayland)" : "");
#endif
}
