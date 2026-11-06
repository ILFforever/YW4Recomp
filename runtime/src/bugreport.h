/* The bug report's files and link (2026-11-07; the overlay's Report a bug pages, gamemenu.c). Saving writes one
 * zip in the reports folder (YW4_REPORT_DIR, default "reports" in the home folder, next to screenshots and logs):
 * report.txt (the player's answers, the version, the GPU and Windows), and as ticked the screenshot (the paused
 * picture, at most 1600 wide, BMP), this run's log and the settings file, and the save folder. The zip stores its
 * files without compression (no library needed; a report is a few MB). Sending opens GitHub's bug form
 * (.github/ISSUE_TEMPLATE/bug.yml) in the browser with the answers filled in; the player drags the zip in. Nothing
 * is uploaded by the game. */
#ifndef YW4_BUGREPORT_H
#define YW4_BUGREPORT_H

#include <stddef.h>
#include <stdint.h>

enum { BUG_ATTACH_SHOT = 1, BUG_ATTACH_LOG = 2, BUG_ATTACH_SAVE = 4 };

typedef struct {
    const char *kind, *often, *text;  /* the answers ("" for none) */
    unsigned attach;                  /* BUG_ATTACH_* */
    const uint8_t *picture;           /* the paused game, RGBA (NULL: none) */
    int picture_w, picture_h;
    int window_w, window_h;
} BugReport;

/* Writes the zip; its full path in path. 0 when it worked. */
int bugreport_save(const BugReport *r, char *path, size_t n);
/* Opens GitHub's bug form with the answers filled in; opens the folder that holds `zip`. */
void bugreport_open_github(const BugReport *r);
void bugreport_show_folder(const char *zip);

#endif
