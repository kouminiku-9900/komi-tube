#include "search.h"

#include <stdio.h>
#include <stdlib.h>

#include "tilefinch/youtube_lite.h"

#include "komi_runtime.h"

struct SearchJob {
    YoutubeLiteLoadJob *load;
};

SearchJob *search_begin(const char *query, char *error, size_t error_size)
{
    char url[1024];
    if (!yt_search_url(query, url, sizeof url)) {
        snprintf(error, error_size, "検索語が空か長すぎます");
        return NULL;
    }
    SearchJob *job = calloc(1, sizeof *job);
    if (job == NULL) return NULL;
    job->load = youtube_lite_load_begin(
        &komi.budget, &komi.session, url, YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES,
        30000, error, error_size);
    if (job->load == NULL) {
        free(job);
        return NULL;
    }
    return job;
}

SearchStatus search_pump(SearchJob *job, YtVideo *videos, size_t *count,
                         char *error, size_t error_size)
{
    YoutubeLiteLoadStatus status = youtube_lite_load_pump(job->load, NULL);
    if (status == YOUTUBE_LITE_LOAD_PENDING) return SEARCH_PENDING;
    YoutubeLiteDocument document = {0};
    if (status != YOUTUBE_LITE_LOAD_SUCCEEDED
        || !youtube_lite_load_take_document(job->load, &document)) {
        snprintf(error, error_size, "%s",
                 youtube_lite_load_error(job->load));
        return SEARCH_FAILED;
    }
    *count = yt_parse_cards(document.html, document.html_length, videos,
                            SEARCH_RESULTS);
    if (*count == 0) {
        /* Keep the document that produced nothing, for the Mac-side
           fixture tests (native/tests/fixtures). */
        char path[256];
        komi_sibling_path(path, sizeof path, "last-empty-search.html");
        FILE *file = fopen(path, "wb");
        if (file != NULL) {
            fwrite(document.html, 1, document.html_length, file);
            fclose(file);
        }
    }
    komi_result("search html=%u source=%u results=%u parsed=%u status=%ld",
                (unsigned) document.html_length,
                (unsigned) document.source_bytes,
                (unsigned) document.result_count, (unsigned) *count,
                document.status_code);
    youtube_lite_document_destroy(&document);
    return SEARCH_DONE;
}

void search_end(SearchJob *job)
{
    if (job == NULL) return;
    youtube_lite_load_destroy(job->load);
    free(job);
}
