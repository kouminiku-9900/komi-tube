/* YouTube search for the native screens, through tilefinch's youtube_lite
 * (the same JSON search API the browser version uses), pumped a slice at a
 * time so the caller can keep drawing. */
#ifndef KOMI_SEARCH_H
#define KOMI_SEARCH_H

#include <stdbool.h>
#include <stddef.h>

#include "yt_results.h"

#define SEARCH_RESULTS 12

typedef struct SearchJob SearchJob;

typedef enum {
    SEARCH_PENDING,
    SEARCH_DONE,
    SEARCH_FAILED
} SearchStatus;

/* NULL (with error filled) when the request could not start. */
SearchJob *search_begin(const char *query, char *error, size_t error_size);
/* Advance a bounded slice. On DONE the videos are in `videos`. */
SearchStatus search_pump(SearchJob *job, YtVideo *videos, size_t *count,
                         char *error, size_t error_size);
void search_end(SearchJob *job);

#endif
