/* Mac-side check of the search path the native app uses:
 *   search_probe <query> [save.html]    live search through youtube_lite
 *   search_probe --file <doc.html>      parse a saved document (fixture)
 * Prints one line per parsed video; exits 1 when nothing was parsed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/session.h"
#include "tilefinch/youtube_lite.h"

#include "yt_results.h"

static int print(const char *html, size_t length)
{
    YtVideo videos[YOUTUBE_LITE_MAXIMUM_RESULTS];
    size_t count = yt_parse_cards(html, length, videos,
                                  YOUTUBE_LITE_MAXIMUM_RESULTS);
    for (size_t i = 0; i < count; i++) {
        printf("%2zu %s [%s] %s / %s / %s\n", i + 1, videos[i].id,
               videos[i].duration, videos[i].title, videos[i].channel,
               videos[i].meta);
    }
    printf("parsed=%zu\n", count);
    return count > 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--file") == 0) {
        FILE *file = fopen(argv[2], "rb");
        if (file == NULL) return 2;
        static char html[YOUTUBE_LITE_MAXIMUM_HTML_BYTES + 1];
        size_t length = fread(html, 1, sizeof html - 1, file);
        fclose(file);
        return print(html, length);
    }
    if (argc < 2) {
        fprintf(stderr, "usage: %s <query> [save.html] | --file <doc>\n",
                argv[0]);
        return 2;
    }
    char url[1024];
    if (!yt_search_url(argv[1], url, sizeof url)) return 2;
    printf("url=%s\n", url);
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    BrowserSession session = {0};
    if (!browser_session_init(&session, &budget, 1024u * 1024u)) return 1;
    YoutubeLiteDocument document = {0};
    char error[256] = {0};
    if (!youtube_lite_load(&budget, &session, url,
                           YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES, 30000,
                           &document, error, sizeof error)) {
        fprintf(stderr, "search failed: %s\n", error);
        return 1;
    }
    printf("html=%zu source=%zu results=%zu status=%ld\n",
           document.html_length, document.source_bytes,
           document.result_count, document.status_code);
    if (argc >= 3) {
        FILE *out = fopen(argv[2], "wb");
        if (out != NULL) {
            fwrite(document.html, 1, document.html_length, out);
            fclose(out);
        }
    }
    int status = print(document.html, document.html_length);
    youtube_lite_document_destroy(&document);
    browser_session_destroy(&session);
    return status;
}
