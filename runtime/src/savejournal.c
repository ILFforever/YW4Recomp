/* Save data's all-or-nothing writes: backups until Commit, put back after a run that stopped mid-save
 * (savejournal.h). */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "host.h"
#include "savejournal.h"

#define BAK ".yw4bak"
#define NEW ".yw4new"
#define PART ".yw4bak.part"

static HostLock lock = HOST_LOCK_INIT;
static char **changed;  /* files changed since the last Commit */
static int nchanged, capchanged;
static char *recovered[8];  /* roots sj_recover already ran on */
static int nrecovered;

static int ends_with(const char *s, const char *end) {
    size_t n = strlen(s), m = strlen(end);
    return n >= m && !strcmp(s + n - m, end);
}

static int exists(const char *p, int *is_dir) {
    struct stat st;
    if (stat(p, &st)) return 0;
    if (is_dir) *is_dir = (st.st_mode & S_IFMT) == S_IFDIR;
    return 1;
}

/* src's bytes into dst, on the disk before dst gets its name (dst.part first, then renamed): a backup cut short
 * never stands in for the real file. */
static int copy_synced(const char *src, const char *dst) {
    char part[1100];
    snprintf(part, sizeof part, "%.1024s%s", dst, ".part");
    FILE *in = fopen(src, "rb"), *out = in ? fopen(part, "wb") : NULL;
    int bad = !in || !out;
    char buf[1 << 16];
    for (size_t n; !bad && (n = fread(buf, 1, sizeof buf, in)) > 0;)
        bad = fwrite(buf, 1, n, out) != n;
    bad = bad || ferror(in) || host_file_sync(out);
    if (in) fclose(in);
    if (out && fclose(out)) bad = 1;
    if (!bad && host_replace_file(part, dst)) bad = 1;
    if (bad) remove(part);
    return bad ? -1 : 0;
}

static int write_mark(const char *p) {
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    int bad = host_file_sync(f);
    return fclose(f) || bad ? -1 : 0;
}

int sj_before_change(const char *path) {
    host_lock(&lock);
    for (int i = 0; i < nchanged; i++)
        if (!strcmp(changed[i], path)) { host_unlock(&lock); return 0; }
    if (nchanged == capchanged) {
        char **grown = realloc(changed, (size_t)(capchanged = capchanged ? 2 * capchanged : 8) * sizeof *changed);
        if (!grown) { host_unlock(&lock); return -1; }
        changed = grown;
    }
    char side[1100];
    int is_dir = 0, r;
    if (exists(path, &is_dir)) {
        snprintf(side, sizeof side, "%.1024s%s", path, BAK);
        r = is_dir ? 0 : copy_synced(path, side);  /* folders aren't journaled: an empty one left over is harmless */
    } else {
        snprintf(side, sizeof side, "%.1024s%s", path, NEW);
        r = write_mark(side);
    }
    if (r) fprintf(stderr, "yw4: save journal: couldn't keep %s's last save (%s); saving without it\n", path, side);
    char *copy = strdup(path);
    if (copy) changed[nchanged++] = copy;
    host_unlock(&lock);
    return r;
}

void sj_commit(void (*each)(const char *path, void *arg), void *arg) {
    host_lock(&lock);
    for (int i = 0; i < nchanged; i++) {
        if (each) each(changed[i], arg);
        char side[1100];
        snprintf(side, sizeof side, "%.1024s%s", changed[i], BAK);
        remove(side);
        snprintf(side, sizeof side, "%.1024s%s", changed[i], NEW);
        remove(side);
        free(changed[i]);
    }
    nchanged = 0;
    host_unlock(&lock);
}

static int recover_dir(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    /* the names first, then the changes: renaming inside a folder being read can skip or repeat entries */
    char **names = NULL;
    int n = 0, cap = 0, put_back = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) {
            char **grown = realloc(names, (size_t)(cap = cap ? 2 * cap : 16) * sizeof *names);
            if (!grown) break;
            names = grown;
        }
        if (!(names[n] = strdup(e->d_name))) break;
        n++;
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char full[1100], orig[1100];
        snprintf(full, sizeof full, "%.1024s/%s", dir, names[i]);
        int is_dir = 0;
        if (!exists(full, &is_dir)) {
        } else if (is_dir) {
            if (depth < 4) put_back += recover_dir(full, depth + 1);
        } else if (ends_with(names[i], PART)) {  /* a backup cut short: the real file was never touched */
            remove(full);
        } else if (ends_with(names[i], BAK)) {
            snprintf(orig, sizeof orig, "%.*s", (int)(strlen(full) - strlen(BAK)), full);
            if (host_replace_file(full, orig) == 0) {
                put_back++;
                fprintf(stderr, "yw4: save journal: the last save didn't finish: %s put back as it was\n", orig);
            } else {
                fprintf(stderr, "yw4: save journal: couldn't put %s back from %s\n", orig, full);
            }
        } else if (ends_with(names[i], NEW)) {
            snprintf(orig, sizeof orig, "%.*s", (int)(strlen(full) - strlen(NEW)), full);
            if (exists(orig, NULL)) {
                remove(orig);
                put_back++;
                fprintf(stderr, "yw4: save journal: the last save didn't finish: %s (new in it) removed\n", orig);
            }
            remove(full);
        }
        free(names[i]);
    }
    free(names);
    return put_back;
}

int sj_recover(const char *root) {
    host_lock(&lock);
    for (int i = 0; i < nrecovered; i++)
        if (!strcmp(recovered[i], root)) { host_unlock(&lock); return 0; }
    if (nrecovered < (int)(sizeof recovered / sizeof *recovered)) recovered[nrecovered++] = strdup(root);
    int n = recover_dir(root, 0);
    host_unlock(&lock);
    return n;
}
