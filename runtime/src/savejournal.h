/* Save data's all-or-nothing writes (review 2026-11-19): the Switch journals a save, so its writes only count once
 * the game commits; a crash, a closed window or a power cut in the middle loses that one save and keeps the last
 * whole one. savefs.c writes into the files themselves (the game reads them back through other handles), so this
 * keeps the old content beside each file instead, until the next Commit:
 *
 *   sj_before_change(path)  before the first change to a file since the last Commit: its old content is copied to
 *                           <path>.yw4bak (synced to the disk first), or, for a file that doesn't exist yet, an empty
 *                           <path>.yw4new marks it as new
 *   sj_commit(...)          at Commit, once the changed files are on the disk: the backups and marks go
 *   sj_recover(root)        once, when a save folder is first opened: whatever a run left behind is undone, each
 *                           backup put back and each new file removed
 *
 * Thread-safe. Paths are host paths, as savefs.c's host_path gives them. */
#ifndef SAVEJOURNAL_H
#define SAVEJOURNAL_H

/* Before the first change to `path` since the last Commit (a no-op after that). 0, or -1 when no backup could be
 * made (disk full...): the change still goes ahead, as before, and the log says so. */
int sj_before_change(const char *path);

/* At Commit, after the changed files are synced: calls each(path) for every file changed since the last Commit
 * (each may be NULL), then removes their backups and marks. */
void sj_commit(void (*each)(const char *path, void *arg), void *arg);

/* Undoes an unfinished save under `root` (folders up to 4 deep). Only the first call per root does anything: later
 * mounts of the same folder in this run must not undo the run's own changes. Returns how many files it put back. */
int sj_recover(const char *root);

#endif
