/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md */
#ifndef PJ_LIVE_SESSION_H
#define PJ_LIVE_SESSION_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint64_t shown,dropped,audio_starves,clock_pts;
    unsigned fps,width,height,refresh_lines; /* refresh_lines 0: firmware automatic */
    int buffering,complete,failed;
    const char *why; /* set once failed */
    /* Last decoder failure (shared/pocket_jpeg.h): 0 none, 1 decoder fault, 2 no
     * progress, 3 bad pixel; pixels reached, fault register 6, ms since the frame began. */
    unsigned jpeg_fail;uint32_t jpeg_pixels,jpeg_faults,jpeg_ms;
    uint64_t rebuffers;int rebuffering; /* 2026-11-10: pauses to refill after the sound ran dry (live_playback.h) */
    unsigned buffered_tenths;uint64_t clipped; /* 2026-11-10 Core Settings: title waiting (0.1 s); samples the gain saturated */
} pj_live_session_stats;
typedef struct {int passed;char message[40];} pj_live_selftest_result;
int pj_live_session_open(void);
void pj_live_session_close(void);
int pj_live_session_push(const uint8_t *packet,size_t size);
int pj_live_session_tick(uint32_t now_us);
void pj_live_session_status(pj_live_session_stats *stats);
/* Decodes an embedded frame on the real FPGA path and checks its pixels. */
void pj_live_session_selftest(pj_live_selftest_result *result);
int pj_live_session_selftest_passed(void);
/* draw fills a 320x30 strip; active decides whether it is shown this frame. */
void pj_live_session_set_osd(void (*draw)(uint16_t *),int (*active)(uint32_t));
/* Film cadence: 1 locks the panel to a whole multiple of the title's frame rate;
 * 0 hands refresh back to the firmware. Returns the new state. */
int pj_live_session_toggle_film(void);
/* Core Settings (pocket/core_settings.h): buffer 0 Short (0.5 s), 1 Long (4 s), 2 Normal (2 s) (live_playback.h
 * PJ_LIVE_BUFFER_*); gain_shift 0, 1 (+6 dB) or 2 (+12 dB). Kept for every later session; applied to the open one. */
void pj_live_session_set_options(unsigned buffer,unsigned gain_shift);
#define test_passed pj_live_session_selftest_passed
#endif
