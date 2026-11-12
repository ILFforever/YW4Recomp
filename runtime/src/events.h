/* What happened in a run, as data (perf.c): one JSON object per line in <perf folder>/perf_<time>.jsonl, next to
 * the per-frame perf_<time>.csv of the same run. The test tools read it instead of searching the log's text
 * (tools/dev/scene.py). Written and flushed as each thing happens, so a run that is killed keeps its lines.
 *
 *     {"kind":"stall","frame":5086,"t":71.204,"ms":52.9,"where":"gpu thread"}
 *
 * kind: screenshot (path), set (name, value: YW4_SET_AT), stall (ms, where; for the GPU thread also pipelines,
 * images and their ms), ab (pass, target, depth, differ, texels: a same-frame A/B's count), fatal (text),
 * unreached (what, frame: scheduled for a frame the run never got to), end (frames). Off with the performance
 * log (YW4_PERF=0). Any thread. */
#ifndef YW4_EVENTS_H
#define YW4_EVENTS_H

#include <stdint.h>

/* `fields`: more members as JSON text, printf style, without braces or a leading comma ("\"ms\":%.1f"); NULL for
 * none. Only for numbers and texts that need no escaping (names of ours). */
void yw4_event(const char *kind, uint64_t frame, const char *fields, ...);
/* One text member, escaped (a path, a message). */
void yw4_event_text(const char *kind, uint64_t frame, const char *key, const char *text);

#endif
