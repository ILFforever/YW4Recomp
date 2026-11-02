/* Which build of YW4 Recomp this is (decision 0047). Defined in <work_dir>/build/version.c, which
 * recomp.build writes on every build: the number from the repo's VERSION file, the git commit it was
 * built from ("+dirty" when there were uncommitted changes; a COMMIT file when there is no git, as in a
 * release download; else "unknown") and the day of the build (UTC). Shown in the log's first lines, the
 * window title and `yw4 --version`. */
#pragma once

extern const char yw4_version[];     /* "0.1.0" */
extern const char yw4_git_commit[];  /* "e5a6351", "e5a6351+dirty" or "unknown" */
extern const char yw4_build_date[];  /* "2026-11-03" */
