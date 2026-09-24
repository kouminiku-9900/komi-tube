/*
 * komi-player -- stage 1: YouTube playback without the browser.
 *
 * Drives tilefinch's media session (resolver -> range reader -> MP4 demux ->
 * Media Engine decode) directly, with no browser engine, page, or chrome.
 * Launching it runs the whole test unattended: every video ID listed in
 * komi-videos.txt beside the program is opened, played for a while, and
 * closed, and one result line per video goes to komi-player.txt beside the
 * program. Under PSPLink that is host0:, so the log lands on the Mac.
 *
 * Nothing here waits for a button. A watchdog thread ends the program if any
 * step stops making progress, so a hang never needs the power switch.
 */
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psputility.h>

#include <malloc.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tilefinch/browser_profile.h"
#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/install_paths.h"
#include "tilefinch/media_backend.h"
#include "tilefinch/platform.h"
#include "tilefinch/psp_display.h"
#include "tilefinch/psp_log.h"
#include "tilefinch/psp_media_present.h"
#include "tilefinch/psp_media_scale.h"
#include "tilefinch/psp_media_session.h"
#include "tilefinch/psp_network.h"
#include "tilefinch/psp_threads.h"
#include "tilefinch/session.h"

PSP_MODULE_INFO("komi_player", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_PRIORITY(TILEFINCH_PSP_THREAD_PRIORITY_BROWSER);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(2560);
PSP_HEAP_SIZE_KB(-1);
PSP_HEAP_THRESHOLD_SIZE_KB(2048);

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 272
#define VRAM_STRIDE 512

#define BUDGET_BYTES (32u * 1024u * 1024u)
#define SESSION_CACHE_BYTES (1024u * 1024u)
#define NETWORK_TIMEOUT_US (30u * 1000u * 1000u)
#define OPEN_TIMEOUT_US (60u * 1000u * 1000u)
#define PLAY_US (30u * 1000u * 1000u)
#define CLOSE_TIMEOUT_US (10u * 1000u * 1000u)
/* No step may go this long without the main loop noting progress. */
#define WATCHDOG_US (120u * 1000u * 1000u)
#define MAX_VIDEOS 32

static PspDisplay display;
static PspMediaSession media;
static PspMediaScaleMap scale_map;
static char log_path[256];
static FILE *result_log;
static volatile uint64_t last_progress_us;
static volatile const char *progress_stage = "boot";

static uint64_t now_us(void)
{
    return (uint64_t) sceKernelGetSystemTimeWide();
}

/* PSP_HEAP_SIZE_KB(-1) hands the whole partition to newlib at start, so the
   kernel's free counters only see what is outside the heap. What the
   program actually holds is the heap's in-use bytes. */
static unsigned heap_used(void)
{
    struct mallinfo info = mallinfo();
    return (unsigned) info.uordblks;
}

static unsigned heap_free(void)
{
    struct mallinfo info = mallinfo();
    return (unsigned) info.fordblks;
}

/* Result lines: short, one per event, flushed at once so a crash keeps them. */
static void result(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (result_log != NULL) {
        fprintf(result_log, "%s\n", line);
        fflush(result_log);
    }
    psp_log_printf("komi-player: %s\n", line);
}

/* tilefinch_core reports through plain printf; send it to the same
   validation log as the media session (linked with --wrap=printf). */
int __wrap_printf(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    psp_log_printf("%s", line);
    return length;
}

static void progress(const char *stage)
{
    progress_stage = stage;
    last_progress_us = now_us();
    psp_log_heartbeat();
}

static int watchdog_thread(SceSize args, void *argp)
{
    (void) args;
    (void) argp;
    for (;;) {
        sceKernelDelayThread(1000 * 1000);
        if (now_us() - last_progress_us > WATCHDOG_US) {
            result("WATCHDOG stage=%s stalled=%llus exiting",
                   (const char *) progress_stage,
                   (unsigned long long) ((now_us() - last_progress_us)
                                         / 1000000u));
            psp_log_finish("watchdog");
            sceKernelExitGame();
        }
    }
    return 0;
}

static void sibling_path(char *out, size_t size, const char *argv0,
                         const char *name)
{
    snprintf(out, size, "%s", argv0 != NULL ? argv0 : "ms0:/");
    char *slash = strrchr(out, '/');
    size_t prefix = slash == NULL ? 0 : (size_t) (slash + 1 - out);
    snprintf(out + prefix, size - prefix, "%s", name);
}

/* --- platform services the media session and transport call back into --- */

static uint64_t platform_now_us(void *context)
{
    (void) context;
    return now_us();
}

static uint64_t platform_now_ns(void *context)
{
    (void) context;
    return now_us() * 1000u;
}

static uint64_t platform_wall_ns(void *context)
{
    (void) context;
    /* src/psp_time.c supplies the RTC-backed time() the TLS stack needs. */
    time_t seconds = time(NULL);
    return seconds <= 0 ? 0 : (uint64_t) seconds * UINT64_C(1000000000);
}

static const char *platform_language(void *context)
{
    (void) context;
    return "ja";
}

static bool platform_cooperate(void *context, const char *phase,
                               size_t completed_work_units)
{
    (void) context;
    (void) phase;
    (void) completed_work_units;
    last_progress_us = now_us();
    return true;
}

static bool media_cancel_requested(void *context)
{
    (void) context;
    return false;
}

static size_t media_free_memory(void *context)
{
    (void) context;
    return (size_t) sceKernelTotalFreeMemSize();
}

static size_t media_largest_block(void *context)
{
    (void) context;
    return (size_t) sceKernelMaxFreeMemSize();
}

static bool media_resolve_offline(void *context, const char *url,
                                  PspMediaOfflineSource *source)
{
    (void) context;
    (void) url;
    (void) source;
    return false;
}

static void media_profile_changed(void *context, uint64_t at_us)
{
    (void) context;
    (void) at_us;
}

static bool media_failure_report(void *context, const char *stage,
                                 const char *detail, const char *url,
                                 long http_status, int native_result)
{
    (void) context;
    (void) url;
    result("failure stage=%s http=%ld native=0x%08x detail=%s",
           stage == NULL ? "?" : stage, http_status,
           (unsigned) native_result, detail == NULL ? "" : detail);
    return true;
}

/* --- presentation: the software scaler into the 16-bit page surface --- */

static void clear_screen(void)
{
    uint16_t *vram = psp_display_back_buffer(&display);
    if (vram == NULL) return;
    memset(vram, 0, (size_t) VRAM_STRIDE * SCREEN_HEIGHT * sizeof(*vram));
    (void) psp_display_publish(&display);
}

static bool present_frame(unsigned *presented)
{
    const MediaVideoFrame *frame = &media.frame;
    if (frame->pixels == NULL || frame->width <= 0 || frame->height <= 0
        || frame->stride_pixels < frame->width || media.playback == NULL)
        return false;
    PspMediaPresentPlan plan;
    if (!psp_media_present_plan(&plan, frame->width, frame->height,
                                frame->stride_pixels, SCREEN_WIDTH,
                                SCREEN_HEIGHT))
        return false;
    PspMediaScaleFormat format = frame->format == MEDIA_PIXEL_RGB565
        ? PSP_MEDIA_SCALE_RGB565 : PSP_MEDIA_SCALE_RGBA8888;
    if (!psp_media_scale_map_matches(&scale_map, format, frame->width,
                                     frame->height, frame->stride_pixels,
                                     plan.video.width, plan.video.height)
        && !psp_media_scale_map_build(&scale_map, format, frame->width,
                                      frame->height, frame->stride_pixels,
                                      plan.video.width, plan.video.height))
        return false;
    /* The scaler reads the decoder's surface, so it must hold the slot the
       same way the browser's presenter does. */
    if (frame->slot >= 0
        && !media_playback_borrow_video_slot(
               media.playback, (unsigned) frame->slot, frame->generation))
        return false;
    uint16_t *vram = psp_display_back_buffer(&display);
    if (vram != NULL) {
        memset(vram, 0, (size_t) VRAM_STRIDE * SCREEN_HEIGHT * sizeof(*vram));
        psp_media_scale_blit(
            &scale_map, frame->pixels,
            vram + (size_t) plan.video.y * VRAM_STRIDE + plan.video.x,
            VRAM_STRIDE);
    }
    if (frame->slot >= 0)
        media_playback_release_video_read(media.playback,
                                          (unsigned) frame->slot);
    if (vram == NULL) return false;
    media_playback_note_frame_displayed(media.playback, frame,
                                        MEDIA_PSP_PRESENT_PATH_SOFTWARE);
    (*presented)++;
    return true;
}

/* One frame of the main loop: advance the session, draw, feed, flip. */
static void pump(uint64_t *last_us, unsigned *presented, uint64_t *identity)
{
    uint64_t now = now_us();
    unsigned elapsed_ms = (unsigned) ((now - *last_us) / 1000u);
    *last_us = now;
    (void) psp_media_advance(&media, elapsed_ms, NULL);
    if (media.frame.pixels != NULL && media.frame.identity != *identity) {
        if (present_frame(presented)) {
            *identity = media.frame.identity;
            (void) psp_media_feed_before_blocking(&media);
            (void) psp_display_publish(&display);
            return;
        }
    }
    (void) psp_media_feed_before_blocking(&media);
    sceDisplayWaitVblankStart();
}

/* --- the test --- */

static size_t load_videos(const char *path, char ids[][16], size_t limit)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) return 0;
    size_t count = 0;
    char line[128];
    while (count < limit && fgets(line, sizeof line, file) != NULL) {
        char *id = line;
        while (*id == ' ' || *id == '\t') id++;
        if (*id == '#' || *id == '\0' || *id == '\n' || *id == '\r') continue;
        size_t length = strcspn(id, " \t\r\n#");
        if (length != 11) continue;
        memcpy(ids[count], id, length);
        ids[count][length] = '\0';
        count++;
    }
    fclose(file);
    return count;
}

static bool try_profile(PspNetwork *network, int profile)
{
    memset(network, 0, sizeof *network);
    uint64_t started = now_us();
    if (!psp_network_begin(network, profile)) {
        result("network FAIL begin profile=%d", profile);
        return false;
    }
    PspNetworkStatus status = network->status;
    while (status != PSP_NETWORK_READY && status != PSP_NETWORK_FAILED
           && status != PSP_NETWORK_CANCELLED) {
        status = psp_network_pump(network, NETWORK_TIMEOUT_US);
        last_progress_us = now_us();
        sceKernelDelayThread(10 * 1000);
    }
    result("network %s profile=%d elapsed=%llums status=%s phase=%s "
           "native=0x%08x apctl=%d wlan-switch=%d wlan-power=%d "
           "heap-used=%u",
           status == PSP_NETWORK_READY ? "READY" : "FAIL", profile,
           (unsigned long long) ((now_us() - started) / 1000u),
           psp_network_status_name(status),
           psp_network_status_name(network->failure_phase),
           (unsigned) network->native_result, network->apctl_state,
           network->wlan_switch_state, network->wlan_power_state,
           heap_used());
    return status == PSP_NETWORK_READY;
}

static char wifi_memo_path[256];

/* No dialog: the firmware Wi-Fi screen would need a button press. Every
   saved connection is listed, then tried in order until one gets an address,
   starting with the one that worked last time (komi-wifi.txt). */
static bool connect_network(PspNetwork *network)
{
    progress("network");
    int remembered = 0;
    FILE *memo = fopen(wifi_memo_path, "r");
    if (memo != NULL) {
        if (fscanf(memo, "%d", &remembered) != 1) remembered = 0;
        fclose(memo);
    }
    int profiles[10];
    size_t count = 0;
    for (int profile = 1; profile <= 10; profile++) {
        if (!psp_network_profile_is_saved(profile)) continue;
        char ssid[40] = {0};
        (void) psp_network_profile_ssid(profile, ssid, sizeof ssid);
        result("wifi-profile %d ssid=\"%s\"", profile, ssid);
        if (profile == remembered && count > 0) {
            profiles[count++] = profiles[0];
            profiles[0] = profile;
        } else {
            profiles[count++] = profile;
        }
    }
    if (count == 0) {
        result("network FAIL no saved Wi-Fi profile");
        return false;
    }
    for (size_t at = 0; at < count; at++) {
        if (try_profile(network, profiles[at])) {
            if (profiles[at] != remembered
                && (memo = fopen(wifi_memo_path, "w")) != NULL) {
                fprintf(memo, "%d\n", profiles[at]);
                fclose(memo);
            }
            /* The browser opens this when its network lifecycle reports
               ready; until then every transport request is refused. */
            fetch_background_transport_set_admission(true);
            return true;
        }
        PspNetworkShutdownReport report;
        psp_network_shutdown(network, &report);
    }
    return false;
}

typedef struct {
    unsigned played;
    unsigned failed;
    unsigned baseline_used;
    unsigned peak_used;
} Totals;

static void play_one(const char *id, unsigned index, uint64_t generation,
                     Totals *totals)
{
    char url[96];
    snprintf(url, sizeof url, "https://www.youtube.com/watch?v=%s", id);
    unsigned used_before = heap_used();
    unsigned used_peak = used_before;
    uint64_t started = now_us();
    progress("open");
    bool accepted = psp_media_open_provider_route(&media, url, generation);

    uint64_t last = now_us();
    uint64_t identity = 0;
    unsigned presented = 0;
    uint64_t first_frame_us = 0;
    uint64_t playing_since = 0;
    uint64_t buffering_us = 0;
    uint64_t max_position_us = 0;
    const char *outcome = accepted ? "timeout" : "refused";
    while (accepted) {
        pump(&last, &presented, &identity);
        progress(playing_since == 0 ? "opening" : "playing");
        uint64_t now = now_us();
        unsigned used_now = heap_used();
        if (used_now > used_peak) used_peak = used_now;
        if (presented > 0 && first_frame_us == 0) first_frame_us = now;
        if (media.ui.playing && playing_since == 0) playing_since = now;
        if (media.ui.buffering && playing_since != 0) buffering_us += 16667u;
        if (media.ui.current_time_us > max_position_us)
            max_position_us = media.ui.current_time_us;
        if (media.ui.failed) { outcome = "failed"; break; }
        if (media.ui.ended) { outcome = "ended"; break; }
        if (playing_since != 0 && now - playing_since >= PLAY_US) {
            outcome = "ok";
            break;
        }
        if (playing_since == 0 && now - started >= OPEN_TIMEOUT_US) break;
    }
    bool ok = strcmp(outcome, "ok") == 0 || strcmp(outcome, "ended") == 0;
    char title[96];
    snprintf(title, sizeof title, "%s", media.ui.title);
    char status[96];
    snprintf(status, sizeof status, "%s", media.ui.status);
    MediaBackendStats stats = {0};
    bool have_stats = psp_media_backend_stats_snapshot(&media, &stats);

    progress("close");
    psp_media_close(&media);
    uint64_t close_started = now_us();
    while ((psp_media_open_work_pending(&media)
            || psp_media_decode_work_pending(&media))
           && now_us() - close_started < CLOSE_TIMEOUT_US) {
        pump(&last, &presented, &identity);
        progress("closing");
    }
    psp_media_pipeline_destroy(&media);
    clear_screen();
    unsigned used_after = heap_used();

    result("video %u id=%s %s outcome=%s first-frame=%llums "
           "position=%llums frames=%u buffering=%llums "
           "heap-used=%u->%u(peak %u) heap-free=%u "
           "decoded=%u dropped=%u title=\"%s\" status=\"%s\"",
           index + 1, id, ok ? "PASS" : "FAIL", outcome,
           (unsigned long long) (first_frame_us == 0
               ? 0 : (first_frame_us - started) / 1000u),
           (unsigned long long) (max_position_us / 1000u),
           presented, (unsigned long long) (buffering_us / 1000u),
           used_before, used_after, used_peak, heap_free(),
           have_stats ? (unsigned) stats.decoded_video_frames : 0u,
           have_stats ? (unsigned) stats.dropped_video_frames : 0u,
           title, status);
    if (ok) totals->played++;
    else totals->failed++;
    if (used_peak > totals->peak_used) totals->peak_used = used_peak;
}

int main(int argc, char **argv)
{
    const char *argv0 = argc > 0 ? argv[0] : NULL;
    last_progress_us = now_us();
    sibling_path(log_path, sizeof log_path, argv0, "komi-player.txt");
    result_log = fopen(log_path, "w");
    (void) psp_log_start(argv0);
    result("start version=2 argv0=%s heap-used=%u heap-free=%u "
           "kernel-free=%u", argv0 == NULL ? "?" : argv0, heap_used(),
           heap_free(), (unsigned) sceKernelTotalFreeMemSize());

    SceUID watchdog = sceKernelCreateThread(
        "komi_watchdog", watchdog_thread, 0x11, 16 * 1024,
        PSP_THREAD_ATTR_USER, NULL);
    if (watchdog >= 0) sceKernelStartThread(watchdog, 0, NULL);

    (void) scePowerSetClockFrequency(333, 333, 166);
    static const TilefinchPlatformServices services = {
        .wall_time_ns = platform_wall_ns,
        .monotonic_time_ns = platform_now_ns,
        .monotonic_time_us = platform_now_us,
        .preferred_language = platform_language,
        .cooperate = platform_cooperate,
    };
    tilefinch_platform_set_services(&services);
    /* Before any other allocation, as the browser does: the decoder's
       working set must sit where the Media Engine can reach it. */
    media_psp_backend_reserve_pool();
    if (!psp_display_begin(&display, psp_display_system_backend()))
        result("display begin failed; continuing without video output");
    clear_screen();

    TilefinchInstallPaths install_paths;
    if (!tilefinch_install_paths_derive(argv0, &install_paths))
        memset(&install_paths, 0, sizeof install_paths);
    char ca_path[256];
    sibling_path(ca_path, sizeof ca_path, argv0, "roots.pem");
    if (!fetch_set_ca_bundle_path(ca_path)) {
        result("FAIL trust bundle %s", ca_path);
        goto finish;
    }

    char list_path[256];
    sibling_path(list_path, sizeof list_path, argv0, "komi-videos.txt");
    static char ids[MAX_VIDEOS][16];
    size_t count = load_videos(list_path, ids, MAX_VIDEOS);
    result("videos=%u list=%s", (unsigned) count, list_path);
    if (count == 0) goto finish;

    sibling_path(wifi_memo_path, sizeof wifi_memo_path, argv0,
                 "komi-wifi.txt");
    PspNetwork network = {0};
    if (!connect_network(&network)) goto finish;

    static Budget budget;
    budget_init(&budget, BUDGET_BYTES);
    static BrowserSession session;
    if (!browser_session_init(&session, &budget, SESSION_CACHE_BYTES)) {
        result("FAIL session init");
        goto finish;
    }
    BrowserProfile *profile = browser_profile_create(&budget);
    if (profile == NULL) {
        result("FAIL profile create");
        goto finish;
    }
    browser_profile_set_youtube_quality(profile, BROWSER_YOUTUBE_QUALITY_360P);
    const PspMediaSessionPlatform platform = {
        .now_us = platform_now_us,
        .cancel_requested = media_cancel_requested,
        .free_memory = media_free_memory,
        .maximum_free_block = media_largest_block,
        .resolve_offline = media_resolve_offline,
        .profile_changed = media_profile_changed,
        .write_failure_report = media_failure_report,
        .install_paths = &install_paths,
    };
    psp_media_init(&media, &budget, &session, profile, NULL, NULL,
                   &platform);

    Totals totals = {.baseline_used = heap_used()};
    totals.peak_used = totals.baseline_used;
    result("ready heap-used=%u heap-free=%u budget=%u",
           totals.baseline_used, heap_free(), (unsigned) budget.current);
    for (size_t at = 0; at < count; at++)
        play_one(ids[at], (unsigned) at, (uint64_t) at + 1u, &totals);

    result("SUMMARY played=%u failed=%u of=%u heap-used baseline=%u "
           "end=%u peak=%u budget-now=%u",
           totals.played, totals.failed, (unsigned) count,
           totals.baseline_used, heap_used(), totals.peak_used,
           (unsigned) budget.current);
    psp_media_shutdown(&media);

finish:
    result("end");
    psp_log_finish("complete");
    if (result_log != NULL) fclose(result_log);
    sceKernelExitGame();
    return 0;
}
