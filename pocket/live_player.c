/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Native Jellyfin client for the paired Pocket media core. All library, artwork
 * and playback logic runs against pj_transport, so the on-card library and the
 * tethered bridge get the same app. No credentials or server addresses reach it.
 *
 * Chromatic tether (Jellymatic's Pocket link docs, phase 2), with no button press:
 *   search   whenever the link is not in use and nothing plays, HELLO every 2 s with a 3 ms
 *            quiet clock before each, so a Chromatic that opens Media later still arms;
 *   linked   a HELLO answer switches to the Chromatic (Home), and a background sync saves
 *            Home, details, folder pages and art (tether_sync.h) for offline use;
 *            HELLO every 5 s while idle; no answer for 9 s (or a failed request) = unplugged;
 *   offline  unplugged: the sync finishes from what is saved, the saved library (tether_cache.h)
 *            is shown with the same screens, titles dimmed, and the search starts again;
 *            replug goes back to the Chromatic on the same title.
 * Where the user stopped and what is watched (watch_state.h) and the saved library are kept
 * in nonvolatile slots 11-19 (tether_store.h), written to the card when the core is quit.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "of_caps.h"
#include "of_video.h"
#include "of_input.h"
#include "of_timer.h"
#include "link_transport.h"
#include "sd_library.h"
#include "live_session.h"
#include "stream_reader.h"
#include "live_ui.h"
#include "diag_log.h"
#include "tether_cache.h"
#include "tether_sync.h"
#include "tether_store.h"
#include "watch_state.h"
#include "save_table.h"
#include "core_settings.h"
#define PJ_DWELL_US 350000u   /* wait before fetching a hero, so scrolling stays cheap */
#define PJ_OSD_US 3500000u
#define PJ_READ_MAX PJ_SD_CHUNK
static pj_rpc_client rpc;
/* What happened, saved to the card (pjlive_log.sav) so a stop can be read on the PC. */
static pj_diag_log diag;
static uint32_t play_started_us,next_stats_us,next_save_us,read_sent_us,read_max_us,read_bytes,reads;
static uint32_t loop_passes,loop_max_us,loop_last_us;
#define DLOG(...) pj_diag_add(&diag,of_time_us(),__VA_ARGS__)
static const char *log_pending; /* a save waiting for the card to go quiet */
static int log_works=-1;          /* -1 untried, 0 failed, 1 ok */
static pj_sd_library stored;
static pj_transport stored_source,*io;
/* The save memory is shared with card reads: only save while none is in flight. */
static int log_quiet(void) {
    if(io==&stored_source&&stored.state!=PJ_SD_IDLE)return 0;
    return pj_sd_io_quiet();
}
static void log_line(void); /* the start screen's log line, kept current */
static uint32_t clock_us(void){return of_time_us();}
/* The Pocket's data-slot table for the two slots this app writes (save_table.h): what the
 * Pocket loaded at start, and after each save the size it will write back at quit. Shown on
 * the start screen and logged, so one run on the device tells whether the exit save works. */
static pj_dt_entry table_log,table_state;
static int table_ok,table_read;   /* ok: 1 both found, 0 a slot missing, -1 no answer from the query path */
static void read_save_table(const char *when) {
#if defined(OF_PC)
    (void)when;   /* desktop checks have no Pocket table */
#else
    if(!of_get_caps()||of_get_caps()->platform_id!=OF_PLATFORM_POCKET||!log_quiet())return;
    int a=pj_dt_find(pj_dt_read_mmio,NULL,PJ_DIAG_SLOT_ID,&table_log);
    int b=pj_dt_find(pj_dt_read_mmio,NULL,PJ_STORE_STATE_SLOT,&table_state);
    table_ok=(a<0||b<0)?-1:(a>0&&b>0);table_read=1;
    DLOG("save table (%s): slot %u entry %d size %lu, slot %u entry %d size %lu%s",when,
        PJ_DIAG_SLOT_ID,table_log.entry,(unsigned long)table_log.size,
        PJ_STORE_STATE_SLOT,table_state.entry,(unsigned long)table_state.size,
        table_ok<0?" (query path gave no answer)":"");
#endif
}
static void save_log(const char *why) {
    if(!log_quiet()){log_pending=why;return;}
    log_pending=NULL;
    uint32_t start=of_time_us();int ok=pj_diag_save(&diag);
    log_works=ok;
    /* Logged after the save, so it goes out with the next one. */
    DLOG("log save (%s) %s in %lums",why,ok?"ok":"FAILED",(unsigned long)((of_time_us()-start)/1000u));
    read_save_table("after log save");log_line();
}
static pj_transport link_source;
static pj_live_library library;
static pj_details details;
static pj_art_cache art_cache;
static pj_art_download art;
static pj_stream_reader stream;
static uint16_t frame[PJ_FB_W*PJ_FB_H];
static uint8_t chunk[PJ_READ_MAX];
static size_t chunk_size,chunk_used;
static uint32_t session,parent,parents[8],pages[8],selected_at[8];
static uint8_t from_home[8]; /* the folder at this depth was opened from Home */
static uint64_t offset;
static unsigned selected,depth,quality=1,action,io_command;
static uint32_t position_seconds,base_seconds,focus_us,osd_until_us;
static int screen=PJ_SCREEN_SOURCE,have_stored,have_link,hud;
static int online,home=1,playing,paused,stream_eof,redraw=1,want_home,want_browse,want_start,want_stop,
    want_details,restart,starting,cancel_start,io_busy;
static char status[80],body[256],diagnostics[5][96];
/* ---- tether state ---- */
static uint8_t cache_bytes[2][PJ_TC_IMAGE_BYTES]; /* the saved image and the one a sync builds */
static pj_tc_image cache_images[2];
static unsigned cache_live;
#define saved_image (&cache_images[cache_live])
static pj_tc_source cache_reader;
static pj_transport cache_source;
static pj_sync tsync;
static pj_store store;
static pj_watch_state watch;
static int have_cache,link_ok,link_streams=1,art_keys;
static int store_reported=1; /* 0: the next finished image save says "Saved for offline" */
enum {TETHER_OFF,TETHER_SEARCH,TETHER_LINKED};
static int tether=TETHER_OFF,search_open;
static uint32_t tether_reply_us,search_at_us;
/* ---- the card's own library next to the Chromatic's (the card's library stays reachable while linked) ----
 * The Chromatic's Home (linked or saved) ends its My Media row with an "On this Pocket" tile when the card has a
 * library (card_tile_add), as the card's own Home does. A on it opens the card's library (the stored source, its
 * titles played from the card); B on the card's Home goes back to the Chromatic. While the user is on the card by
 * choice (card_visit), a Chromatic the search finds no longer takes the screen over: an answered HELLO only says it
 * is there (header "Chromatic connected"), the link is closed and asked again 2 s later, so the Chromatic keeps
 * seeing the Pocket. The link and its protocol are unchanged; while a card title plays the search waits, as it
 * always has while anything plays. */
#define PJ_CARD_TILE_HANDLE 0xffffffffu
#define PJ_CARD_TILE_TITLE "On this Pocket"
static int card_visit,chromatic_here,card_folder_pending; /* pending: open the card's own "On this Pocket" folder */
static uint32_t chromatic_seen_us;
enum {OWNER_APP,OWNER_SYNC,OWNER_ALIVE,OWNER_PROBE,OWNER_NONE}; /* NONE: a re-sync with no HELLO after it */
static int io_owner;
/* ---- link rate (link_hw.h; Jellymatic's Pocket link docs "Link rate fallback") ----
 * search   a HELLO that gets no answer is asked again at the next slower rate (256, 64, 16,
 *          8 kHz, round again); the rate that answered is tried first for the rest of the run.
 *          Before every HELLO the clock is held high and still for 4 ms, so the Chromatic's
 *          receiver finds the byte boundary again.
 * climb    once linked, the next faster rate is tried with a probe: HELLOs until 600 bytes of
 *          answers have arrived, every frame of them with a good checksum. Only then is the
 *          rate kept. One damaged frame, or no answer within a second, goes straight back,
 *          and the link stays up. (A cable too slow for the rate damages bytes toward the
 *          Pocket without any fault bit or timeout: only the checksum sees it.)
 * use      three damaged frames at a rate above the one the search found: one step down,
 *          between two requests, also while playing.
 * play     512 kHz carries every profile, 256 kHz up to 30 FPS (a 60 FPS choice is played at
 *          30); below 256 kHz the library and pictures work and playing is refused in words.
 * A core without the slow rates (word 7 reads 0) searches at 256 kHz only. */
#define PJ_LINK_PROBE_US 1000000u
#define PJ_LINK_PROBE_BYTES 600u  /* answers received, all checksums good, before a faster rate is kept */
#define PJ_LINK_PROBE_ASKS 16u
#define PJ_LINK_DRAIN_US 300000u
#define PJ_LINK_RETRY_CLIMB_US 60000000u
static const unsigned search_rates[]={256u,64u,16u,8u};
static unsigned search_list[5],search_count=1,search_step,search_tries;
static unsigned link_good_khz;                    /* the rate the last search found the Chromatic at */
static unsigned link_want_khz=PJ_LINK_SAFE_KHZ;   /* what the Chromatic says it can follow, capped by the link speed setting */
static unsigned link_offer_khz=PJ_LINK_SAFE_KHZ;  /* what it said, uncapped (a new setting caps it again) */
static unsigned link_ceiling_khz=4000u;           /* a probe above this failed */
static unsigned probe_from_khz,gate_khz;
static int search_armed,gate,gate_owner;          /* gate: 1 waiting for the wire to empty, 2 clock held still */
static uint32_t gate_us,probe_deadline_us,climb_retry_us,climb_wait_us=PJ_LINK_RETRY_CLIMB_US;
static uint32_t found_bad,found_faults,alive_count;
/* The step-down count (3 damaged frames or fault reads since found_bad/found_faults) is a count
 * of trouble close together: after a minute with neither, it starts again from nothing. */
#define PJ_LINK_CLEAN_US 60000000u
static uint32_t seen_bad,seen_faults,clean_us;
static uint32_t probe_bad,probe_got;static unsigned probe_asks; /* the probe: frames damaged and bytes received at its start */
static unsigned stream_quality=1;                 /* the profile the running stream was asked at */
static int play_pending,play_from_start;          /* Play pressed on a link too slow to play: the climb first */
static unsigned read_resyncs;                     /* the stream was started again after "Read out of step." */
static uint32_t first_data_us;static int got_data;/* when this stream's first data arrived */
static int want_reload;                           /* the Chromatic's stream capability changed: list again */
#define PJ_BUFFER_LIMIT_US 10000000u              /* first data to playing; longer is a fault in words */
#define PJ_NO_DATA_LIMIT_US 30000000u             /* START answered to first data */
static char link_lines[3][64],link_sign[72];
static pj_link_numbers link_shown;static unsigned link_shown_state=~0u;
/* A sign shown for a while (found, saved, unplugged), then the steady state shows again. */
static char sign_text[72],temp_sign[72],shown_sign[72];
static int temp_tone,sign_tone_now;static uint32_t temp_until_us;static const char *sign_hint;
static unsigned shown_done,shown_total,progress_done,progress_total;
static uint32_t focus_key;static int focus_details;
/* Next episode at the end of an episode: 5 s, B cancels. */
#define PJ_NEXT_EPISODE_US 5000000u
static int countdown_item=-1;static uint32_t countdown_until_us;static char countdown_text[40];
/* "Log run 3 saved; table 10:e6 1234 11:e7 80": run = how many starts the log on the card
 * has seen (1 = the card still holds the seed, so no earlier quit wrote it back); the table
 * part is the Pocket's slot table, the entry and the byte count it will write at quit. */
static void table_text(char *out,size_t n) {
    if(!table_read){snprintf(out,n,"table: not read");return;}
    if(table_ok<0){snprintf(out,n,"table: no answer");return;}
    if(table_log.entry<0&&table_state.entry<0){snprintf(out,n,"table: slots missing");return;}
    /* entries are -1..63, sizes at most the 256 KiB slot: 35 characters at most */
    snprintf(out,n,"table %u:e%d %u %u:e%d %u",PJ_DIAG_SLOT_ID,table_log.entry%100,(unsigned)table_log.size%1000000u,
        PJ_STORE_STATE_SLOT,table_state.entry%100,(unsigned)table_state.size%1000000u);
}
/* The header's plain-words form: "Save sizes: log 587 B, place 32768 B". The log is written at
 * start, so its size should already be small and non-zero; "place" is the watch state, 32768 (the
 * seed) until the first B, then the bytes the Pocket writes at quit. "missing" = the Pocket gave
 * that slot no table entry (it cannot be saved); "no answer" = the table could not be read. */
static void size_words(char *out,size_t n,const pj_dt_entry *e) {
    if(!table_read)snprintf(out,n,"not read");
    else if(table_ok<0)snprintf(out,n,"no answer");
    else if(e->entry<0)snprintf(out,n,"missing");
    else snprintf(out,n,"%u B",(unsigned)e->size%1000000u);
}
static void log_line(void) {
    char table[40];table_text(table,sizeof(table));
    unsigned run=(unsigned)diag.boot%100000u;
    if(log_works>0)snprintf(diagnostics[4],sizeof(diagnostics[4]),"Log run %u saved; %s",run,table);
    else if(log_works<0)snprintf(diagnostics[4],sizeof(diagnostics[4]),"Log run %u waiting for the card; %s",run,table);
    else snprintf(diagnostics[4],sizeof(diagnostics[4]),"Log step %u err %u; %s",   /* step 1 open, 2 write, 3 close */
        (unsigned)diag.stage%10u,(unsigned)diag.err%10000u,table);
}
static const char *profiles[]={"pocket-link-jpeg-min-24","pocket-link-jpeg-low-30","pocket-link-jpeg-motion-60"};
static const char *quality_names[]={"24 FPS","30 FPS","60 FPS"};
static uint32_t page;
static void set_status(const char *s){snprintf(status,sizeof(status),"%.79s",s);redraw=1;}
/* A time kept in the 32-bit microsecond clock is due unless it is 1..span ahead of now. The
 * clock wraps every 71 minutes: with a signed compare a time left over from long ago holds
 * things back (or keeps them on) for half a wrap; this way a stale time is simply due. */
static int due(uint32_t now,uint32_t at,uint32_t span){return (uint32_t)(at-now)-1u>=span;}
#define PJ_SEARCH_SPAN_US 4000000u /* longer than any wait the search or a probe sets */
static void osd_show(uint32_t now){osd_until_us=now+PJ_OSD_US;if(!osd_until_us)osd_until_us=1;}
static void reset_art(void){memset(&art_cache,0,sizeof(art_cache));art.handle=0;art.complete=0;}
static void focus_changed(uint32_t now){focus_us=now;action=0;redraw=1;}
static void disconnect(void) {
    if(io)io->close(io->context);
    io=NULL;online=io_busy=0;
}
static void flash_sign(const char *text,int tone,uint32_t seconds) {
    snprintf(temp_sign,sizeof(temp_sign),"%s",text);temp_tone=tone;
    temp_until_us=of_time_us()+seconds*1000000u;if(!temp_until_us)temp_until_us=1;redraw=1;
}
/* The search order: the rate that answered last time first, then fast to slow. */
static void search_plan(void) {
    search_count=0;
    if(link_good_khz>=pj_ln.slowest_khz)search_list[search_count++]=link_good_khz;
    for(unsigned i=0;i<4;i++) {
        unsigned khz=search_rates[i];
        if(khz<pj_ln.slowest_khz||(search_count&&khz==search_list[0]))continue;
        search_list[search_count++]=khz;
    }
    if(!search_count)search_list[search_count++]=PJ_LINK_SAFE_KHZ;
    search_step=0;
}
/* The link numbers, for the log (the screen has them too: link_text). */
static void log_link(const char *what) {
    char words[48];pj_link_fault_words(pj_ln.faults,words,sizeof(words));
    if(pj_link_cfg.acks) /* with ACKs (Auto, Safe, an ACK test build) it also says whether the Chromatic asked and how many went */
        DLOG("link %s: %u kHz, try %u step %u of %u, sent %lu got %lu, frames %lu good %lu bad, faults %s (0x%x, %lu times), acks %s %lu",
            what,pj_ln.khz,search_tries,search_step+1,search_count,(unsigned long)pj_ln.sent,(unsigned long)pj_ln.received,
            (unsigned long)pj_ln.good,(unsigned long)pj_ln.bad,words,pj_ln.faults,(unsigned long)pj_ln.fault_events,
            pj_ln.ack_on?"on":"off",(unsigned long)pj_ln.acks);
    else
        DLOG("link %s: %u kHz, try %u step %u of %u, sent %lu got %lu, frames %lu good %lu bad, faults %s (0x%x, %lu times)",
            what,pj_ln.khz,search_tries,search_step+1,search_count,(unsigned long)pj_ln.sent,(unsigned long)pj_ln.received,
            (unsigned long)pj_ln.good,(unsigned long)pj_ln.bad,words,pj_ln.faults,(unsigned long)pj_ln.fault_events);
}
static void search_restart(uint32_t now) {
    if(!link_ok)return;
    tether=TETHER_SEARCH;search_open=search_armed=0;gate=0;pj_ln.hold=0;
    search_plan();search_at_us=now+PJ_TETHER_GAP_US;
}
static void lib_reset(void);
static void fault(const char *message) {
    pj_live_session_close();disconnect();lib_reset();card_visit=card_folder_pending=0;
    playing=paused=starting=cancel_start=0;session=0;chunk_size=chunk_used=0;
    want_home=want_browse=want_start=want_stop=want_details=restart=0;play_pending=want_reload=0;countdown_item=-1;
    screen=PJ_SCREEN_SOURCE;set_status(message);search_restart(of_time_us());
    DLOG("fault: %s",message);save_log("fault");
}
/* Names the check that stopped playback, so a report from the device is useful. */
static void note_position(uint32_t seconds,int finished);
static void fault_playback(const char *what) {
    pj_live_session_stats stats;pj_live_session_status(&stats);
    char message[80];
    if(playing&&library.count){uint64_t at=base_seconds+stats.clock_pts/90000;note_position((uint32_t)(at<172800?at:172799),0);}
    static const char *const jpeg_why[]={"","decoder fault","no progress","bad pixel"};
    if(stats.jpeg_fail&&stats.jpeg_fail<4)
        DLOG("decoder detail: %s, %lu of %ux%u pixels, faults 0x%lx, %lums into the frame",
            jpeg_why[stats.jpeg_fail],(unsigned long)stats.jpeg_pixels,stats.width,stats.height,
            (unsigned long)stats.jpeg_faults,(unsigned long)stats.jpeg_ms);
    /* The numbers are on screen too: if the log cannot reach the card, a photo of the
     * screen still says which check fired (f = fault register 6, p = pixels, t = ms). */
    if(stats.why&&*stats.why&&stats.jpeg_fail&&stats.jpeg_fail<4)
        snprintf(message,sizeof(message),"%s (%s) at %lus [f%02lx p%lu t%lu]",what,jpeg_why[stats.jpeg_fail],
            (unsigned long)(base_seconds+stats.clock_pts/90000),(unsigned long)stats.jpeg_faults,
            (unsigned long)stats.jpeg_pixels,(unsigned long)stats.jpeg_ms);
    else if(stats.why&&*stats.why)snprintf(message,sizeof(message),"%s: %s at %lus",what,stats.why,
        (unsigned long)(base_seconds+stats.clock_pts/90000));
    else snprintf(message,sizeof(message),"%s at %lus",what,(unsigned long)(base_seconds+stats.clock_pts/90000));
    fault(message);
}
static const char *unavailable_note(void) {
    if(io==&cache_source)return "Stream it when the Chromatic is connected";
    if(io==&link_source&&!link_streams)return "This Chromatic cannot stream yet";
    return NULL;
}
/* The stream profile for a link rate and the user's 24/30/60 choice. shared/pocket_tether.h has
 * a set of profiles for each rate that plays, sized to what that rate really brings end to end
 * (the budget is there); a 60 FPS choice on a 256 kHz link is asked as 30. NULL below 256 kHz. */
static const unsigned quality_fps[]={24u,30u,60u};
static const pj_tether_profile *stream_profile; /* what the running stream was asked at (link source) */
static const pj_tether_profile *link_profile(unsigned khz){return pj_tether_profile_for(khz,quality_fps[quality]);}
/* The frame-rate choice a stream is really asked at. The user's choice itself is kept. */
static unsigned quality_now(void) {
    if(io!=&link_source)return quality;
    const pj_tether_profile *p=link_profile(pj_ln.khz);
    return !p?0u:p->fps>=60u?2u:p->fps>=30u?1u:0u;
}
static void fill_ui(pj_ui_state *s) {
    *s=(pj_ui_state){screen,paused,selected,action,&library,&art_cache,&details,status,
        io?io->label:"",io&&!io->stored?quality_names[quality_now()]:"",position_seconds,
        {diagnostics[0],diagnostics[1],diagnostics[2],diagnostics[3],diagnostics[4]},have_stored||have_cache,have_link,
        shown_sign,sign_hint,progress_done,progress_total,sign_tone_now,unavailable_note(),
        countdown_item>=0?countdown_text:NULL,countdown_item>=0&&countdown_item<(int)library.count?
            (library.items[countdown_item].subtitle[0]?library.items[countdown_item].subtitle:library.items[countdown_item].title):NULL,
        have_cache?"A  Saved from your Chromatic":"A  Library on this Pocket",
        have_cache&&have_stored?"Select  On this Pocket (SD card)":NULL,
        {link_lines[0],link_lines[1],link_lines[2]}};
}
static void show_ui(void) {
    pj_ui_state s;fill_ui(&s);
    pj_ui_draw(frame,&s);
    of_video_mode_t mode;of_video_get_mode(&mode);uint8_t *fb=of_video_surface();
    if(mode.stride<640)return;
    for(unsigned y=0;y<PJ_FB_H;y++)memcpy(fb+y*mode.stride,frame+y*PJ_FB_W,640);
    of_video_flip();redraw=0;
}
static int request(unsigned command,const char *json,uint32_t now) {
    if(!io||!io->begin(io->context,command,json,now))return 0;
    io_command=command;io_busy=1;io_owner=OWNER_APP;
    if(command==PJ_RPC_READ)read_sent_us=now;
    return 1;
}
/* Reopening the link restores 256 kHz and a quiet clock; the request numbers carry on, so a
 * Chromatic never mistakes a new request for a replay of an old one. */
static int link_open(void) {
    uint32_t sequence=rpc.sequence;
    if(!pj_link_transport_open(&link_source,&rpc,(volatile uint32_t *)PJ_LINK_BASE))return 0;
    rpc.sequence=sequence;return 1;
}
/* ---- link rate: re-sync, climb, too slow to play ---- */
static int link_slow(void){return io==&link_source&&!pj_link_can_play();}
static void slow_status(void) {
    snprintf(status,sizeof(status),"Link too slow to play (%u kHz): browsing only",pj_ln.khz);redraw=1;
}
/* A clean start for the Chromatic's receiver before a HELLO on the open link: nothing more is
 * sent, the wire empties, the clock stays high and still for 4 ms (at a new rate when one is
 * asked: soft reset, rate, clock driven high), then the HELLO goes out. Both orders are the
 * ones proven against the Chromatic bridge RTL (a test bench in the Jellymatic repository). */
static void gate_begin(unsigned khz,int owner,uint32_t now) {
    gate=1;gate_khz=khz;gate_owner=owner;gate_us=now;
    pj_ln.hold=1;pj_hw_mode(&rpc.link,0);
}
/* The faster rate did not answer: straight back to the one that works, and stay there for a
 * while (one minute, doubling each time, at most 16). The link stays up. */
static void probe_failed(uint32_t now,const char *why) {
    log_link(why);
    pj_rpc_cancel(&rpc);io_busy=0;io_owner=OWNER_APP;
    link_ceiling_khz=probe_from_khz;
    climb_retry_us=now+climb_wait_us;if(!climb_retry_us)climb_retry_us=1;
    if(climb_wait_us<16u*PJ_LINK_RETRY_CLIMB_US)climb_wait_us*=2u;
    tether_reply_us=now;
    gate_begin(probe_from_khz,OWNER_ALIVE,now); /* the HELLO that follows proves the old rate again */
}
static void gate_tick(uint32_t now) {
    if(!gate)return;
    if(io!=&link_source||tether!=TETHER_LINKED){gate=0;pj_ln.hold=0;return;}
    if(gate==1) {
        int quiet=pj_hw_quiet(&rpc.link);
        if(!quiet&&(uint32_t)(now-gate_us)<PJ_LINK_DRAIN_US+pj_link_bytes_us(8u))return;
        if(!quiet||gate_khz!=pj_ln.khz) {
            pj_hw_reset(&rpc.link);pj_ln.khz=pj_hw_rate(&rpc.link,gate_khz);pj_hw_mode(&rpc.link,0);
        }
        gate=2;gate_us=now;return;
    }
    if((uint32_t)(now-gate_us)<PJ_LINK_RESYNC_US)return;
    gate=0;pj_ln.hold=0;pj_hw_mode(&rpc.link,1);
    if(gate_owner==OWNER_PROBE&&pj_ln.khz!=gate_khz) { /* this core does not run that rate */
        link_ceiling_khz=pj_ln.khz;return;
    }
    if(gate_owner==OWNER_NONE)return;
    if(request(PJ_RPC_HELLO,"{}",now)) {
        io_owner=gate_owner;
        if(gate_owner==OWNER_PROBE) {
            probe_bad=pj_ln.bad;probe_got=pj_ln.received;probe_asks=1;
            probe_deadline_us=now+PJ_LINK_PROBE_US+pj_link_bytes_us(21u+512u);
        }
    } else if(gate_owner==OWNER_PROBE)probe_failed(now,"probe could not be sent");
}
/* The next rate to try: doubling up to 512 kHz, then what the Chromatic said it follows. */
static unsigned climb_next(void) {
    unsigned top=link_want_khz<link_ceiling_khz?link_want_khz:link_ceiling_khz,khz=pj_ln.khz,next;
    if(khz>=top)return 0;
    next=khz<512u?khz*2u:top;
    return next>top?top:next;
}
/* The link was lost at this rate with three or more damaged frames or faults counted since it
 * was found: the next search starts one step slower. The first try back up comes after a short
 * fixed wait (10 s, not the doubling minute a failed probe earns): if the rate really is bad the
 * probe fails and that rule takes over. */
#define PJ_LINK_DEMOTE_RETRY_US 10000000u
static int link_demoted; /* the last loss stepped the search down: its 10 s wait stands at "found" */
static void link_demote(uint32_t now) {
    link_demoted=1;
    unsigned khz=pj_ln.khz;
    if(link_good_khz&&khz>link_good_khz)link_ceiling_khz=khz/2u>link_good_khz?khz/2u:link_good_khz;
    else for(unsigned i=0;i<4;i++)if(search_rates[i]<khz&&search_rates[i]>=pj_ln.slowest_khz) {
        link_good_khz=link_ceiling_khz=search_rates[i];break;
    }
    climb_retry_us=now+PJ_LINK_DEMOTE_RETRY_US;if(!climb_retry_us)climb_retry_us=1;
}
/* The link lines for the start screen (and the header of the library screens while the search
 * runs): rate, search step, bytes, frames, faults. Rebuilt only when a number changes. */
static void link_text(void) {
    unsigned state=(unsigned)tether|(unsigned)(io==&link_source)<<2|(unsigned)link_ok<<3|search_step<<4|search_count<<8|search_tries<<12;
    if(state==link_shown_state&&link_shown.sent==pj_ln.sent&&link_shown.received==pj_ln.received&&
       link_shown.good==pj_ln.good&&link_shown.bad==pj_ln.bad&&link_shown.fault_events==pj_ln.fault_events&&
       link_shown.khz==pj_ln.khz&&link_shown.faults==pj_ln.faults)return;
    link_shown=pj_ln;link_shown_state=state;
    link_lines[0][0]=link_lines[1][0]=link_lines[2][0]=link_sign[0]=0;
    if(!link_ok)return;
    unsigned tries=search_tries%100000u;
    const char *faults=pj_ln.faults?"":", no faults";
    if(tether==TETHER_LINKED)snprintf(link_lines[0],sizeof(link_lines[0]),"Link %u kHz: %s, %lu CRC errors",pj_ln.khz,
        pj_link_can_play()?"connected":"browsing only",(unsigned long)(pj_ln.bad%1000000u));
    else if(pj_ln.slowest_khz>=PJ_LINK_SAFE_KHZ)snprintf(link_lines[0],sizeof(link_lines[0]),
        "Link 256 kHz only (older core), try %u%s",tries,faults);
    else snprintf(link_lines[0],sizeof(link_lines[0]),"Link %u kHz, try %u, step %u of %u%s",pj_ln.khz,tries,
        search_step+1,search_count,faults);
    snprintf(link_lines[1],sizeof(link_lines[1]),"Sent %lu B, got %lu B; frames %lu good, %lu bad",
        (unsigned long)(pj_ln.sent%100000000u),(unsigned long)(pj_ln.received%100000000u),
        (unsigned long)(pj_ln.good%1000000u),(unsigned long)(pj_ln.bad%1000000u));
    if(pj_ln.faults) {
        char words[48];pj_link_fault_words(pj_ln.faults,words,sizeof(words));
        snprintf(link_lines[2],sizeof(link_lines[2]),"Faults x%lu: %s",(unsigned long)(pj_ln.fault_events%100000u),words);
    } else if(tether==TETHER_LINKED)snprintf(link_lines[2],sizeof(link_lines[2]),"Faults: none");
    if(tether==TETHER_SEARCH)snprintf(link_sign,sizeof(link_sign),"Chromatic search: %u kHz, try %u, got %lu B",
        pj_ln.khz,tries,(unsigned long)(pj_ln.received%100000000u));
    else if(tether==TETHER_LINKED&&!pj_link_can_play())snprintf(link_sign,sizeof(link_sign),"Link %u kHz: browsing only",pj_ln.khz);
    if(screen==PJ_SCREEN_SOURCE)redraw=1;
}
static void connect_source(pj_transport *source,uint32_t now) {
    pj_live_session_close();disconnect();
    if(source==&cache_source&&(!have_cache||!pj_tc_source_open(&cache_source,&cache_reader,saved_image)))
        {set_status("No saved library yet");return;}
    if(!source->context){set_status("That source is not available");return;}
    io=source;reset_art();lib_reset();session=0;playing=paused=starting=cancel_start=0;chunk_size=chunk_used=0;
    want_home=want_browse=want_start=want_stop=want_details=restart=0;play_pending=want_reload=0;countdown_item=-1;
    if(!request(PJ_RPC_HELLO,"{}",now)){fault("Could not start that source");return;}
    set_status("Opening library...");
}
/* The "On this Pocket" tile at the end of the Chromatic's My Media row (its own row at the end when there is no
 * My Media), on every Home that is not the card's own, when the card has a library. A opens it (card_open). */
static void card_tile_add(void) {
    if(!have_stored||io==&stored_source||library.count>=PJ_HOME_ITEMS)return;
    unsigned r=0;
    while(r<library.row_count&&strcmp(library.rows[r].title,"My Media"))r++;
    unsigned at;
    if(r<library.row_count)at=library.rows[r].first+library.rows[r].count;
    else {
        if(library.row_count>=PJ_HOME_ROWS)return;
        r=library.row_count++;at=library.count;
        pj_library_row *row=&library.rows[r];
        snprintf(row->title,sizeof(row->title),"%s",PJ_CARD_TILE_TITLE);row->first=at;row->count=0;row->shape=PJ_ART_THUMB;
    }
    memmove(&library.items[at+1],&library.items[at],(library.count-at)*sizeof(library.items[0]));
    for(unsigned k=r+1;k<library.row_count;k++)library.rows[k].first++;
    library.rows[r].count++;library.count++;
    pj_library_item *tile=&library.items[at];memset(tile,0,sizeof(*tile));
    tile->handle=PJ_CARD_TILE_HANDLE;tile->folder=1;tile->card_tile=1;
    snprintf(tile->title,sizeof(tile->title),"%s",PJ_CARD_TILE_TITLE);
    unsigned titles=(unsigned)stored.title_count;
    snprintf(tile->subtitle,sizeof(tile->subtitle),"%u video%s on the SD card",titles,titles==1?"":"s");
}
/* X on the start screen: ask the Chromatic now instead of at the next search window. */
static void search_now(uint32_t now) {
    if(!link_ok){set_status("Media core required for the bridge");return;}
    if(tether==TETHER_OFF){tether=TETHER_SEARCH;search_open=search_armed=0;search_plan();}
    if(tether==TETHER_SEARCH&&!search_open&&!search_armed)search_at_us=now;
    set_status("");flash_sign("Asking the Chromatic...",PJ_SIGN_BUSY,3);
}
/* Remember what is on screen, so the next source can show the same title. */
static void keep_focus(void) {
    focus_key=0;focus_details=0;
    if(!library.count||selected>=library.count||(screen!=PJ_SCREEN_HOME&&screen!=PJ_SCREEN_DETAILS))return;
    const pj_library_item *item=&library.items[selected];
    focus_key=item->folder?pj_sync_folder_key(item):pj_watch_key(item);
    focus_details=screen==PJ_SCREEN_DETAILS&&!item->folder;
}
static void restore_focus(void) {
    if(!focus_key)return;
    for(unsigned i=0;i<library.count;i++) {
        const pj_library_item *item=&library.items[i];
        if((item->folder?pj_sync_folder_key(item):pj_watch_key(item))!=focus_key)continue;
        selected=i;
        if(focus_details){screen=PJ_SCREEN_DETAILS;action=0;details.valid=0;want_details=1;}
        break;
    }
    focus_key=0;focus_details=0;redraw=1;
}
/* Where playback stopped, for this run and the next: the item (in every row it appears in),
 * the progress bars, Details and Resume all use it. */
static void note_position(uint32_t seconds,int finished) {
    if(!library.count||selected>=library.count)return;
    pj_library_item *item=&library.items[selected];
    pj_watch_entry *e=pj_watch_note(&watch,item,seconds,finished);
    if(!e)return;
    uint32_t key=e->key;
    for(unsigned i=0;i<library.count;i++)if(pj_watch_key(&library.items[i])==key) {
        library.items[i].resume_ticks=(uint64_t)e->position_s*10000000u;library.items[i].played=e->played;
    }
    pj_store_save_state(&store);redraw=1;
}
static void stop_at_position(int resume_after) {
    pj_live_session_stats stats;pj_live_session_status(&stats);
    uint64_t seconds=base_seconds+stats.clock_pts/90000;
    position_seconds=(uint32_t)(seconds<172800?seconds:172799);
    note_position(position_seconds,0);
    want_stop=session!=0;restart=resume_after;playing=0;chunk_size=chunk_used=0;
    DLOG("stop at %lus shown %lu drop %lu gaps %lu refills %lu clipped %lu resume=%d",(unsigned long)position_seconds,
        (unsigned long)stats.shown,(unsigned long)stats.dropped,(unsigned long)stats.audio_starves,
        (unsigned long)stats.rebuffers,(unsigned long)stats.clipped,resume_after);
    pj_live_session_close();redraw=1;
    if(!resume_after)log_pending="stop"; /* a READ may still be in flight */
}
/* The image a sync just finished becomes the saved library, and goes to the card. */
static void sync_commit(void) {
    if(tsync.state!=PJ_SY_DONE||!tsync.next->ready)return;
    cache_live=(unsigned)(tsync.next-cache_images);tsync.state=PJ_SY_IDLE;
    have_cache=pj_tc_find(saved_image,PJ_RPC_HOME,0,0,0)!=NULL;
    /* Nothing changed since the last save (a re-sync of the same Home): no write, no sign. */
    if(saved_image->used==store.card_size&&pj_store_crc(saved_image->data,saved_image->used)==store.card_crc&&!store.image_pending)
        DLOG("sync done: unchanged, nothing to save");
    else {
        /* Say "Saved for offline" when something new was saved, not on every Home reload. */
        int news=tsync.fresh||store.card_size==0;
        pj_store_save_image(&store,saved_image);store_reported=!news;
    }
    snprintf(diagnostics[3],sizeof(diagnostics[3]),"Saved from your Chromatic: %u tiles",pj_tc_count(saved_image,PJ_RPC_ART));
    DLOG("sync done: %u tiles, %u fetched (%lu bytes), %u reused, %u skipped, %u backdrops, %lu of %lu bytes used%s",
        tsync.total,tsync.fetched,(unsigned long)tsync.fetched_bytes,tsync.reused,tsync.skipped,tsync.heroes,
        (unsigned long)saved_image->used,(unsigned long)saved_image->capacity,tsync.offline?", finished offline":"");
    redraw=1;
}
/* Finish a sync without the link: everything already saved is kept, nothing more is asked. */
static void sync_finish_offline(void) {
    if(tsync.state!=PJ_SY_RUNNING)return;
    pj_sync_offline(&tsync);
    while(pj_sync_next(&tsync)!=PJ_SY_FINISHED){}
    sync_commit();
}
static void sync_start(const uint8_t *home_json,size_t size) {
    sync_finish_offline(); /* a new Home renumbers every tile: keep what the last one fetched */
    unsigned build=cache_live^1u;
    pj_tc_init(&cache_images[build],cache_bytes[build],sizeof(cache_bytes[build]));
    if(!pj_sync_begin(&tsync,&cache_images[build],have_cache?saved_image:NULL,home_json,size,art_keys)) {
        tsync.state=PJ_SY_IDLE;DLOG("sync could not start");return;
    }
    DLOG("sync start: %u tiles, saved library %s",tsync.total,have_cache?"present":"none");
}
/* A START the Chromatic refuses with "Busy." is asked again before the title fails: a request it
 * gave up on may still be out at its server. Six more tries 0.7 s apart (4.2 s), never back to
 * back. No other refusal is asked again: "fast connection failed" lasts for the Chromatic's whole
 * link session, so waiting cannot clear it. */
#define PJ_START_RETRIES 6u
#define PJ_START_RETRY_US 700000u
static unsigned start_refusals;static uint32_t start_hold_us;
/* The same for the library: a Home, folder page, details or picture (the app's or the sync's)
 * refused "Busy." is asked again 0.7 s later, at most six more times, then the refusal stands in
 * the Chromatic's words. Found again while its launcher still waits on a server request the
 * Pocket gave up on, the first Home is refused that way until the late answer is in. While the
 * 0.7 s run, only the library requests wait: buttons, keep-alives, START and READ go on. */
static unsigned lib_refusals;static uint32_t lib_hold_us;
static int lib_waiting(uint32_t now){return lib_hold_us&&!due(now,lib_hold_us,PJ_START_RETRY_US);}
static void lib_reset(void){lib_refusals=0;lib_hold_us=0;}
static int lib_busy_again(const char *why) {
    if(io!=&link_source||tether!=TETHER_LINKED||strncmp(why,"Busy",4)||lib_refusals>=PJ_START_RETRIES){lib_reset();return 0;}
    lib_refusals++;lib_hold_us=of_time_us()+PJ_START_RETRY_US;if(!lib_hold_us)lib_hold_us=1;
    DLOG("library request refused busy (%u), asked again in 0.7 s",lib_refusals);
    return 1;
}
static void open_selected(uint32_t now);
static int handle_reply(unsigned command,const uint8_t *p,size_t n) {
    pj_json j;uint64_t value;
    if(command==PJ_RPC_HELLO) {
        char encoding[32];
        if(!pj_json_parse(&j,p,n,library.tokens,4096)||
           !pj_json_u64(&j,pj_json_find(&j,0,"version"),&value)||value!=1||
           !pj_json_text(&j,pj_json_find(&j,0,"compressed_stream"),encoding,sizeof(encoding)))return 0;
        int streams=!strcmp(encoding,"PJAV-JPEG-IMA");
        /* A Chromatic without the stream yet still serves the library; other sources must stream. */
        if(!streams&&(io!=&link_source||strcmp(encoding,"none")))return 0;
        if(io==&link_source) {
            /* A peer that can follow a faster clock says so. The rate is not taken on its word:
             * the climb tries it with one HELLO and goes straight back if that is not answered. */
            link_offer_khz=PJ_LINK_SAFE_KHZ;
            if(pj_json_u64(&j,pj_json_find(&j,0,"link_khz"),&value)&&value>=PJ_LINK_SAFE_KHZ&&value<=4000)
                link_offer_khz=(unsigned)value;
            /* link_hw.h: the top rate, or the cap of the Link speed setting (or of a diagnostic build) */
            link_want_khz=link_offer_khz>pj_link_cfg.max_khz?pj_link_cfg.max_khz:link_offer_khz;
            link_streams=streams;art_keys=pj_json_true(&j,pj_json_find(&j,0,"art_keys"));
            /* 2026-11-10: a launcher from 1.5.0 takes ACKs in place of the filler (rpc_client.h pj_rpc_fill); an
             * older one would count them as refused frames, so they go only where asked for */
            pj_ln.ack_on=pj_json_u64(&j,pj_json_find(&j,0,"credit_ack"),&value)&&value==1;
            tether=TETHER_LINKED;tether_reply_us=of_time_us();
            flash_sign("Chromatic found: loading Home",PJ_SIGN_BUSY,30);
            DLOG("chromatic found: stream %s, art keys %d",encoding,art_keys);
        }
        online=1;want_home=1;set_status(io==&link_source?"":"Loading Home...");return 1;
    }
    if(command==PJ_RPC_HOME||command==PJ_RPC_BROWSE) {
        if(!pj_library_load(&library,p,n,command==PJ_RPC_HOME))return 0;
        /* The saved library and a Chromatic without the stream show the titles, dimmed. */
        if(io==&cache_source||(io==&link_source&&!link_streams))
            for(unsigned i=0;i<library.count;i++)if(!library.items[i].folder)library.items[i].unavailable=1;
        pj_watch_apply(&watch,&library);
        if(command==PJ_RPC_HOME)card_tile_add();
        if(selected>=library.count)selected=0;
        home=command==PJ_RPC_HOME;screen=home?PJ_SCREEN_HOME:PJ_SCREEN_LIBRARY;
        focus_changed(focus_us);
        set_status(library.count?"":"Nothing here yet; Y: Home");
        if(home&&io==&link_source&&tether==TETHER_LINKED) {
            if(!strncmp(temp_sign,"Chromatic found",15))temp_until_us=0; /* Home is here: the sync's progress takes over */
            sync_start(p,n);
        }
        restore_focus();
        /* Opened from the Chromatic's "On this Pocket" tile: straight into the card's own folder of that name (its
         * My Media tile, tools/pocket_library.py finish_home), B going back to the card's Home. A card without it
         * stays on its Home. */
        if(home&&io==&stored_source&&card_folder_pending) {
            card_folder_pending=0;
            for(unsigned i=0;i<library.count;i++)if(library.items[i].folder&&!strcmp(library.items[i].title,PJ_CARD_TILE_TITLE))
                {selected=i;open_selected(focus_us);break;}
        }
        return 1;
    }
    if(command==PJ_RPC_DETAILS) {
        if(library.count)pj_details_load(&details,library.tokens,library.items[selected].handle,p,n);
        redraw=1;return 1;
    }
    if(command==PJ_RPC_ART) {
        if(!art.handle)return 1; /* superseded while in flight */
        if(!pj_art_chunk(&art,p,n))return 0;
        if(art.complete){pj_sync_offer(&tsync,&art);pj_art_store(&art_cache,&art);art.handle=0;redraw=1;}
        return 1;
    }
    if(command==PJ_RPC_START) {
        if(!pj_json_parse(&j,p,n,library.tokens,4096)||
           !pj_json_u64(&j,pj_json_find(&j,0,"session"),&value)||!value||value>0xffffffffu)return 0;
        session=(uint32_t)value;
        starting=0;start_refusals=0;
        if(cancel_start){cancel_start=0;want_stop=1;return 1;}
        if(!pj_live_session_open()){want_stop=1;playing=0;set_status("JPEG/audio runtime unavailable");return 1;}
        pj_stream_reader_init(&stream,session);offset=0;stream_eof=0;chunk_size=chunk_used=0;
        /* Stored media starts at the nearest seek point, not the exact request. */
        uint64_t actual;
        if(pj_json_u64(&j,pj_json_find(&j,0,"start_seconds"),&actual))position_seconds=(uint32_t)actual;
        base_seconds=position_seconds;playing=1;paused=0;set_status("");
        play_started_us=next_stats_us=of_time_us();next_save_us=play_started_us+2000000u;got_data=0;
        read_max_us=read_bytes=reads=0;
        DLOG("play handle %lu from %lus session %lu source %s",(unsigned long)library.items[selected].handle,
            (unsigned long)position_seconds,(unsigned long)session,io&&io->stored?"card":"link");
        osd_show(of_time_us());return 1;
    }
    if(command==PJ_RPC_READ) {
        if(!playing||want_stop)return 1; /* stop/seek can supersede an in-flight read */
        if(n<16||pj_av_u32(p)!=session||pj_av_u64(p+4)!=offset||p[13]||
           pj_av_u16(p+14)!=n-16||n-16>sizeof(chunk))return 0;
        if(p[12]==1) {
            if(n==16)return 0;
            chunk_size=n-16;chunk_used=0;memcpy(chunk,p+16,chunk_size);offset+=chunk_size;
            if(!got_data){got_data=1;first_data_us=of_time_us();}
            uint32_t waited=of_time_us()-read_sent_us;
            if(waited>read_max_us)read_max_us=waited;
            read_bytes+=(uint32_t)chunk_size;reads++;
        } else if(p[12]==2) {if(n!=16)return 0;}
        else if(p[12]==3) {
            if(n!=16||!pj_stream_reader_eof(&stream))return 0;
            stream_eof=1;
        } else return 0;
        return 1;
    }
    if(command==PJ_RPC_STOP) {
        if(!pj_json_parse(&j,p,n,library.tokens,4096)||!pj_json_true(&j,pj_json_find(&j,0,"stopped")))return 0;
        session=0;want_stop=0;chunk_size=chunk_used=0;
        if(restart){want_start=1;restart=0;}return 1;
    }
    return 0;
}
static void leave_folder(void);
/* Why the source refused, in its own words: the Chromatic answers {"error":"..."}. "" if none. */
static void refusal_reason(char *why,size_t size) {
    why[0]=0;
    if(io!=&link_source)return;
    size_t n=0;const uint8_t *p=io->reply(io->context,&n);pj_json j;
    if(!pj_json_parse(&j,p,n,library.tokens,4096)||!pj_json_text(&j,pj_json_find(&j,0,"error"),why,size))why[0]=0;
}
/* The reason on the status line: "Chromatic: <reason>" if that fits the line, else the reason
 * alone, else (or with no reason) the plain words. */
static void refusal_status(const char *why,const char *plain) {
    char words[112];snprintf(words,sizeof(words),"Chromatic: %s",why);
    set_status(!why[0]?plain:pj_ui_measure(words)<=272?words:pj_ui_measure(why)<=272?why:plain);
}
/* A refused request leaves the source healthy: say so and stay put. */
static void handle_refused(unsigned command) {
    DLOG("refused: command %u",command);
    if(command==PJ_RPC_START) {
        int cancelled=cancel_start,again=restart;starting=cancel_start=0;restart=0;
        char why[96];refusal_reason(why,sizeof(why)); /* the Chromatic says why in words */
        if(io==&link_source)DLOG("start refused (%u): %s",start_refusals+1u,why[0]?why:"no reason given");
        if(cancelled&&!again) { /* B while it was being asked, or the link became too slow to play */
            start_refusals=0;if(!strncmp(status,"Cancelling",10))set_status("");
            return;
        }
        /* Asked again a few times before the title fails, but only the refusal that waiting can
         * clear: "Busy." with a request the Chromatic gave up on. The status line stays
         * "Starting...", B cancels, and the library, pictures and link numbers go on in between.
         * Every other reason ("Unknown media handle", "fast connection failed", which the
         * Chromatic keeps until its link is opened again, a stream that could not be opened, no
         * reason at all) is shown at once. */
        int clears=!strncmp(why,"Busy",4);
        if(io==&link_source&&tether==TETHER_LINKED&&clears&&start_refusals<PJ_START_RETRIES) {
            start_refusals++;want_start=1;
            start_hold_us=of_time_us()+PJ_START_RETRY_US;if(!start_hold_us)start_hold_us=1;
            return;
        }
        start_refusals=0;
        if(io!=&link_source)set_status("This title is not on this Pocket");
        else refusal_status(why,"The Chromatic could not start this title");
    } else if(command==PJ_RPC_READ) {
        /* The Chromatic refuses every READ of a stream it cannot use (not an AVI, a picture size
         * the Pocket cannot show, ended before its headers) or no longer has. Asking again would
         * go round for ever with nothing on screen: stop, back to Details, its reason in words. */
        if(!playing||want_stop)return;
        char why[96];refusal_reason(why,sizeof(why));
        DLOG("read refused: %s",why[0]?why:"no reason given");
        if(!strcmp(why,"Read out of step.")&&!read_resyncs) {
            /* The two ends disagree about the place in the stream. Once (until a minute has
             * played), the stream is started again at the place on screen. */
            read_resyncs=1;stop_at_position(1);set_status("Starting again at the same place...");return;
        }
        stop_at_position(0);paused=0;screen=PJ_SCREEN_DETAILS;action=0;
        if(io!=&link_source)set_status("This source stopped the title");
        else refusal_status(why,"The Chromatic stopped this title");
    } else if(command==PJ_RPC_STOP) {
        /* Nothing left to stop on the other side is the same as stopped: never asked again. */
        session=0;want_stop=0;chunk_size=chunk_used=0;
        if(restart){want_start=1;restart=0;}
    } else if(command==PJ_RPC_HOME||command==PJ_RPC_BROWSE||command==PJ_RPC_DETAILS||command==PJ_RPC_ART) {
        char why[96];refusal_reason(why,sizeof(why));
        if(lib_busy_again(why)) { /* asked again once the 0.7 s are over (schedule) */
            if(command==PJ_RPC_HOME)want_home=1;
            else if(command==PJ_RPC_BROWSE)want_browse=1;
            else if(command==PJ_RPC_DETAILS)want_details=1;
            return; /* a picture: art.handle stays, so the same piece is asked again */
        }
        if(command==PJ_RPC_ART) {
            if(art.handle){art.total=0;art.used=0;pj_art_store(&art_cache,&art);art.handle=0;}
        } else if(command==PJ_RPC_DETAILS)redraw=1;
        else if(command==PJ_RPC_BROWSE) {
            if(io==&link_source)refusal_status(why,"That folder is not on this Pocket");
            else set_status("That folder is not on this Pocket");
            if(depth)leave_folder();
            if(selected>=library.count)selected=0;
        } else if(io==&link_source)refusal_status(why,"Not available from this source");
        else set_status("Not available from this source");
    } else set_status("Not available from this source");
}
static int art_needed(uint32_t handle,unsigned kind) {
    return kind==PJ_ART_BACKDROP?!pj_art_find_hero(&art_cache,handle):!pj_art_find_card(&art_cache,handle,kind);
}
/* Linked: a picture the saved library (or the running sync) already has under the same art key
 * is shown from memory instead of being fetched again. */
static int art_from_saved(const pj_library_item *item,unsigned kind,unsigned row_shape) {
    if(io!=&link_source||!art_keys)return 0;
    uint32_t key=kind==PJ_ART_BACKDROP?item->hero_key:kind==row_shape?item->art_key:0;
    if(!key)return 0;
    const pj_tc_image *image=NULL;const pj_tc_record *r=NULL;
    if(tsync.state==PJ_SY_RUNNING&&(r=pj_tc_find_key(tsync.next,PJ_RPC_ART,kind,key))!=NULL)image=tsync.next;
    else if(have_cache&&(r=pj_tc_find_key(saved_image,PJ_RPC_ART,kind,key))!=NULL)image=saved_image;
    if(!r||r->size!=pj_art_width(kind)*pj_art_height(kind)*2u)return 0;
    pj_art_begin(&art,item->handle,kind);memcpy(art.bytes,pj_tc_blob(image,r),r->size);
    art.total=art.used=r->size;art.complete=1;pj_art_store(&art_cache,&art);art.handle=0;redraw=1;
    return 1;
}
static int next_art(uint32_t now) {
    if(!library.count)return 0;
    const pj_library_row *row=&library.rows[home?pj_ui_focused_row(&library,selected):0];
    unsigned shape=row->shape,row_shape=shape,first,last;
    if(screen==PJ_SCREEN_LIBRARY) {
        unsigned columns=shape==PJ_ART_THUMB?3u:4u,focus_row=selected/columns;
        first=(focus_row?focus_row-1:0)*columns;last=first+columns*3;
    } else if(screen==PJ_SCREEN_DETAILS){first=selected;last=selected+1;shape=PJ_ART_POSTER;}
    else {
        unsigned visible=shape==PJ_ART_THUMB?3u:4u,column=selected-row->first;
        first=row->first+(column>=visible?column-(visible-1):0);last=first+visible+1;
        if(last>row->first+row->count)last=row->first+row->count;
    }
    if(last>library.count)last=library.count;
    for(unsigned i=first;i<last;i++)if(!library.items[i].card_tile&&art_needed(library.items[i].handle,shape)) {
        if(art_from_saved(&library.items[i],shape,row_shape))continue;
        pj_art_begin(&art,library.items[i].handle,shape);return 1;
    }
    /* The "On this Pocket" tile is the app's own: no source has a picture for it. */
    if(screen!=PJ_SCREEN_LIBRARY&&!library.items[selected].card_tile&&art_needed(library.items[selected].handle,PJ_ART_BACKDROP)&&
       !art_from_saved(&library.items[selected],PJ_ART_BACKDROP,row_shape)&&(uint32_t)(now-focus_us)>=PJ_DWELL_US) {
        pj_art_begin(&art,library.items[selected].handle,PJ_ART_BACKDROP);return 1;
    }
    return 0;
}
/* Start the selected title: schedule() asks it as a START. */
static void play_begin(int from_start,uint32_t now) {
    pj_library_item *item=&library.items[selected];
    position_seconds=from_start?0:(uint32_t)(item->resume_ticks/10000000);
    paused=0;want_start=1;start_refusals=0;start_hold_us=0;read_resyncs=0;focus_us=now;set_status("Starting...");
    if(quality_now()!=quality) { /* the link rate carries less than the user's choice: say what plays */
        snprintf(status,sizeof(status),"Starting at %s (link %u kHz)...",quality_names[quality_now()],pj_ln.khz);redraw=1;
    }
}
/* A later HELLO answer (the keep-alive every few seconds, a probe). What the Chromatic said when
 * it was found is not for the whole session: it says "none" only once its fast connection has
 * failed, which can happen after the first HELLO, and then keeps saying it until its link is
 * opened again (Media left and entered); the new link can stream again. When the answer changes,
 * the list is asked again, so the titles dim or light up. */
static void hello_again(const uint8_t *p,size_t n) {
    pj_json j;char encoding[32];
    if(io!=&link_source||!pj_json_parse(&j,p,n,library.tokens,4096)||
       !pj_json_text(&j,pj_json_find(&j,0,"compressed_stream"),encoding,sizeof(encoding)))return;
    int streams=!strcmp(encoding,"PJAV-JPEG-IMA");
    if(!streams&&strcmp(encoding,"none"))return;
    if(streams==link_streams)return;
    link_streams=streams;want_reload=1;
    DLOG("chromatic stream now %s",encoding);
}
/* The wait before the next try at a faster rate is over: the ceiling goes. Looked at on every
 * pass, linked or not, and when the Chromatic is found: a time left standing while unplugged
 * would be "ahead" again once the 32-bit clock has gone half way round. */
static void climb_due(uint32_t now) {
    if(climb_retry_us&&due(now,climb_retry_us,16u*PJ_LINK_RETRY_CLIMB_US)){climb_retry_us=0;link_ceiling_khz=4000u;}
}
/* A minute with no damaged frame and no fault read: the step-down count starts from nothing. */
static void link_clean_tick(uint32_t now) {
    if(io!=&link_source||tether!=TETHER_LINKED)return;
    if(pj_ln.bad!=seen_bad||pj_ln.fault_events!=seen_faults) {
        seen_bad=pj_ln.bad;seen_faults=pj_ln.fault_events;clean_us=now+PJ_LINK_CLEAN_US;return;
    }
    if(due(now,clean_us,PJ_LINK_CLEAN_US)) {
        found_bad=pj_ln.bad;found_faults=pj_ln.fault_events;clean_us=now+PJ_LINK_CLEAN_US;
    }
}
/* One step down to `down` between two requests (also while playing: the stream starts again at the slower rate's
 * profile, at the same place; below 256 kHz it stops with the reason on screen). `failed`: the rate damaged frames,
 * so the climb waits (a minute, doubling); else the Link speed setting asked for a slower rate, and its cap holds. */
static void link_step_down(unsigned down,int failed,uint32_t now) {
    log_link(failed?"stepping down":"stepping down (link speed setting)");
    found_bad=pj_ln.bad;found_faults=pj_ln.fault_events;
    if(failed) {
        link_ceiling_khz=down;
        climb_retry_us=now+climb_wait_us;if(!climb_retry_us)climb_retry_us=1;
        if(climb_wait_us<16u*PJ_LINK_RETRY_CLIMB_US)climb_wait_us*=2u;
    }
    const pj_tether_profile *lower=link_profile(down);
    if((playing||starting)&&lower!=stream_profile) {
        int again=lower!=NULL;
        if(playing)stop_at_position(again);
        if(starting){cancel_start=1;restart=again;}
        if(again)set_status(stream_profile&&lower->fps<stream_profile->fps?
            "Link slower: playing at a lower frame rate":"Link slower: lower picture quality");
        else snprintf(status,sizeof(status),"Link too slow to play (%u kHz): browsing only",down);
        redraw=1;
    }
    gate_begin(down,OWNER_NONE,now);
}
/* ---- Core Settings (pocket/core_settings.h): the Pocket menu's playback choices ----
 * Looked at four times a second; a field the menu gives replaces the one in hand, a field of 0 changes nothing. */
static pj_core_settings core_set;
static uint32_t settings_seen,settings_check_us;
static int link_setting_pending; /* the link speed changed while linked: schedule() moves the rate between requests */
static void settings_apply(unsigned changed,const char *why) {
    if(!changed)return;
    char words[80];pj_settings_text(&core_set,words,sizeof(words));
    DLOG("core settings (%s): %s, word 0x%08lx%s",why,words,(unsigned long)pj_settings_pack(&core_set),
         PJ_LINK_BUILD_FIXED?"; link speed fixed by this build":"");
    if(changed&(1u<<PJ_SET_FPS)) {
        /* A fixed rate is the app's 24/30/60 choice (Select still changes it until the menu does again); the title's
         * own rate is the app's own default, 30 (a 24 fps film plays at 24: the rate is a ceiling). */
        unsigned fps=pj_settings_fps(&core_set),want=fps==24u?0u:fps==60u?2u:1u;
        if(want!=quality&&online&&io&&!io->stored) { /* as Select: the stream starts again at the new rate */
            if(playing)stop_at_position(1);
            if(starting){cancel_start=1;restart=1;}
        }
        quality=want;redraw=1;
    }
    if((changed&(1u<<PJ_SET_LINK))&&!PJ_LINK_BUILD_FIXED) {
        pj_link_cfg=pj_link_mode_for(core_set.code[PJ_SET_LINK]);
        link_want_khz=link_offer_khz>pj_link_cfg.max_khz?pj_link_cfg.max_khz:link_offer_khz;
        if(link_want_khz<PJ_LINK_SAFE_KHZ)link_want_khz=PJ_LINK_SAFE_KHZ;
        link_setting_pending=1;
    }
    if(changed&((1u<<PJ_SET_BUFFER)|(1u<<PJ_SET_VOLUME)))
        pj_live_session_set_options(pj_settings_buffer_mode(&core_set),pj_settings_gain_shift(&core_set));
    /* Kept with the watch state, so the next start has every field even if the Pocket writes only some. */
    pj_core_settings defaults=pj_settings_defaults();
    uint32_t word=pj_settings_pack(&core_set),kept=store.settings?store.settings:pj_settings_pack(&defaults);
    if(word!=kept){store.settings=word;pj_store_save_state(&store);}
}
static void settings_poll(uint32_t now) {
    if(!due(now,settings_check_us,250000u))return;
    settings_check_us=now+250000u;if(!settings_check_us)settings_check_us=1;
    uint32_t word;
    if(!pj_settings_read(&word)||word==settings_seen)return;
    settings_seen=word;
    settings_apply(pj_settings_merge(&core_set,word),"menu");
}
static void settings_start(void) {
    core_set=pj_settings_defaults();settings_seen=0;
    if(store.settings)pj_settings_merge(&core_set,store.settings);
    uint32_t word=0;
    if(pj_settings_read(&word)){settings_seen=word;pj_settings_merge(&core_set,word);}
    settings_apply((1u<<PJ_SET_FIELDS)-1u,store.settings?"start, kept and menu":"start, menu");
}
static void schedule(uint32_t now) {
    if(!online||io_busy||gate||!io->idle(io->context))return;
    if(want_stop) {
        snprintf(body,sizeof(body),"{\"session\":%lu}",(unsigned long)session);
        request(PJ_RPC_STOP,body,now);return;
    }
    if(io==&link_source&&tether==TETHER_LINKED) {
        /* The Link speed setting changed: a slower cap moves the rate now, a faster one lets the climb try it. */
        if(link_setting_pending) {
            link_setting_pending=0;
            if(pj_ln.khz>link_want_khz){link_step_down(link_want_khz,0,now);return;}
            if(pj_ln.khz<link_want_khz){link_ceiling_khz=4000u;climb_retry_us=0;climb_wait_us=PJ_LINK_RETRY_CLIMB_US;}
        }
        /* A rate the probe passed that then damages frames is given up at once, between two
         * requests (also while playing: the next READ simply goes out at the slower rate). */
        if(link_good_khz&&pj_ln.khz>link_good_khz&&
           (pj_ln.bad-found_bad>=3u||pj_ln.fault_events-found_faults>=3u)) {
            unsigned down=pj_ln.khz/2u>link_good_khz?pj_ln.khz/2u:link_good_khz;
            link_step_down(down,1,now);return;
        }
        climb_due(now);
        /* The climb: only with nothing playing or starting, one step at a time. */
        unsigned next=climb_next();
        if(next&&!playing&&!paused&&!starting&&!want_start&&!restart&&!session) {
            probe_from_khz=pj_ln.khz;gate_begin(next,OWNER_PROBE,now);return;
        }
        /* Play was pressed on a link too slow to play, and the climb it asked for is over. */
        if(play_pending) {
            play_pending=0;
            if(pj_link_can_play())play_begin(play_from_start,now);else slow_status();
        }
        /* The list is asked again where it is on screen; elsewhere it waits. */
        if(want_reload&&(screen==PJ_SCREEN_HOME||screen==PJ_SCREEN_LIBRARY)&&
           !playing&&!paused&&!starting&&!want_start&&!restart&&!want_details) {
            want_reload=0;keep_focus();if(home)want_home=1;else want_browse=1;
        }
    }
    /* The 0.7 s after a library request was refused "Busy.": the library waits, nothing else. */
    int lib_wait=lib_waiting(now);
    if(!lib_wait)lib_hold_us=0;
    /* A list asked now is the new one: a reload still waiting would only load it twice. */
    if(want_home&&!lib_wait) {want_home=0;want_reload=0;home=1;depth=0;parent=page=0;art.handle=0;request(PJ_RPC_HOME,"{}",now);return;}
    if(want_browse&&!lib_wait) {
        want_browse=0;want_reload=0;art.handle=0;snprintf(body,sizeof(body),"{\"handle\":%lu,\"start\":%lu}",
            (unsigned long)parent,(unsigned long)page);request(PJ_RPC_BROWSE,body,now);return;
    }
    /* The wait after a refusal: only while the retry time is at most one wait ahead, so a time
     * left over from an old try can never hold a START back. */
    if(want_start&&library.count&&(!start_hold_us||(uint32_t)(start_hold_us-now)>PJ_START_RETRY_US)) {
        want_start=0;start_hold_us=0;
        if(link_slow()){paused=0;start_refusals=0;slow_status();return;}
        const char *name=profiles[stream_quality=quality_now()];
        if(io==&link_source&&(stream_profile=link_profile(pj_ln.khz))!=NULL)name=stream_profile->name;
        snprintf(body,sizeof(body),"{\"handle\":%lu,\"profile\":\"%s\",\"start_seconds\":%lu}",
            (unsigned long)library.items[selected].handle,name,(unsigned long)position_seconds);
        if(stream_quality!=quality)DLOG("link %u kHz: %s asked, playing at %s",pj_ln.khz,quality_names[quality],quality_names[stream_quality]);
        starting=request(PJ_RPC_START,body,now);return;
    }
    if(playing) {
        if(!stream_eof&&!chunk_size) {
            snprintf(body,sizeof(body),"{\"session\":%lu,\"offset\":%llu}",
                (unsigned long)session,(unsigned long long)offset);request(PJ_RPC_READ,body,now);
        }
        return;
    }
    if(want_details&&library.count&&!lib_wait) {
        want_details=0;snprintf(body,sizeof(body),"{\"handle\":%lu}",
            (unsigned long)library.items[selected].handle);request(PJ_RPC_DETAILS,body,now);return;
    }
    if(!lib_wait&&(art.handle||next_art(now))) {
        snprintf(body,sizeof(body),"{\"handle\":%lu,\"kind\":%u,\"offset\":%lu}",
            (unsigned long)art.handle,art.kind,(unsigned long)art.used);request(PJ_RPC_ART,body,now);return;
    }
    if(io!=&link_source||tether!=TETHER_LINKED||starting||want_start||restart)return;
    /* Idle on the Chromatic: a keep-alive now and then proves the cable is still in. */
    if((uint32_t)(now-tether_reply_us)>=PJ_TETHER_ALIVE_US) {
        gate_begin(pj_ln.khz,OWNER_ALIVE,now); /* a still clock first, then the HELLO */
        return;
    }
    if(tsync.state!=PJ_SY_RUNNING||lib_wait)return;
    int step=pj_sync_next(&tsync);
    if(step==PJ_SY_REQUEST){if(request(tsync.command,tsync.body,now))io_owner=OWNER_SYNC;else tsync.waiting=0;}
    else if(step==PJ_SY_FINISHED)sync_commit();
}
/* ---- the Chromatic: search, unplug ---- */
/* Unplugged (silence or a failed request): keep the place, finish the sync from what is saved,
 * show the saved library and look for the Chromatic again. */
static void tether_lost(uint32_t now,const char *why) {
    DLOG("chromatic lost: %s",why);log_link("lost");
    /* A plain loss (cable pulled, Chromatic off) says nothing against the rate: the Pocket is the
     * clock master, an empty cable reads as idle bytes and at most cuts one frame short. Only the
     * count that steps a working link down (3 damaged frames or faults since "found") moves the
     * next search a step slower; otherwise it starts at the rate that worked. */
    if(pj_ln.bad-found_bad>=3u||pj_ln.fault_events-found_faults>=3u)link_demote(now);
    if(playing)stop_at_position(0);
    else if(starting||paused)note_position(position_seconds,0);
    keep_focus();
    if(screen==PJ_SCREEN_DETAILS||playing||paused||starting)focus_details=focus_key!=0;
    pj_live_session_close();disconnect();lib_reset();
    playing=paused=starting=cancel_start=0;session=0;chunk_size=chunk_used=0;
    want_home=want_browse=want_start=want_stop=want_details=restart=0;play_pending=want_reload=0;countdown_item=-1;
    sync_finish_offline();
    search_restart(now);
    if(have_cache||have_stored) {
        uint32_t key=focus_key;int show=focus_details;
        connect_source(have_cache?&cache_source:&stored_source,now);
        focus_key=key;focus_details=show;
        flash_sign("Chromatic unplugged: showing saved library",PJ_SIGN_WARN,8);
    } else {
        screen=PJ_SCREEN_SOURCE;set_status("");
        flash_sign("Chromatic unplugged",PJ_SIGN_WARN,8);
    }
    save_log("unplugged");
}
/* Found while on the start screen or the saved library: switch to the Chromatic. The HELLO
 * reply in the link buffer is handled as the new source's first answer. */
static void tether_found(uint32_t now) {
    const uint8_t *p;size_t n=0;p=link_source.reply(link_source.context,&n);
    static uint8_t hello[1024];
    if(n>sizeof(hello)) {
        /* An answer this app cannot hold (a newer launcher on the Chromatic): say so; the search
         * goes on and says it again each time, so the words stay up. */
        DLOG("chromatic hello too long: %lu bytes",(unsigned long)n);
        flash_sign("Update the Pocket app: Chromatic is newer",PJ_SIGN_WARN,8);
        link_source.close(link_source.context);search_open=0;search_at_us=now+PJ_TETHER_PING_US;return;
    }
    memcpy(hello,p,n);
    link_good_khz=pj_ln.khz;found_bad=seen_bad=pj_ln.bad;found_faults=seen_faults=pj_ln.fault_events;
    clean_us=now+PJ_LINK_CLEAN_US;
    /* A new link session (a cable put back, perhaps another cable): the wait a failed probe left
     * (up to 16 minutes after a few failures) is not this cable's, so it goes with its ceiling;
     * one probe of 1 s says again whether the faster rate works. Only the 10 s a step down after a
     * doubtful loss set (link_demote) stands. */
    if(link_demoted)climb_due(now);
    else{climb_retry_us=0;link_ceiling_khz=4000u;climb_wait_us=PJ_LINK_RETRY_CLIMB_US;}
    link_demoted=0;log_link("found");
    keep_focus();
    uint32_t key=focus_key;int show=focus_details;
    if(io){pj_live_session_close();io->close(io->context);io=NULL;online=io_busy=0;}
    io=&link_source;reset_art();lib_reset();session=0;playing=paused=starting=cancel_start=0;chunk_size=chunk_used=0;
    want_home=want_browse=want_start=want_stop=want_details=restart=0;play_pending=want_reload=0;countdown_item=-1;search_open=0;
    if(!handle_reply(PJ_RPC_HELLO,hello,n)){fault("Invalid reply from the Chromatic");return;}
    focus_key=key;focus_details=show;tether_reply_us=now;set_status("");
}
/* A on the "On this Pocket" tile: the card's library, by the user's choice. A sync in progress keeps what it has
 * (as when the cable goes); the link goes back to the search, whose answers only say the Chromatic is there. */
static void card_open(uint32_t now) {
    int linked=io==&link_source&&tether==TETHER_LINKED;
    DLOG("card library opened from %s",linked?"the Chromatic":"the saved library");
    if(io==&link_source)sync_finish_offline();
    connect_source(&stored_source,now);
    if(io!=&stored_source)return;
    card_visit=card_folder_pending=1;
    if(linked){search_restart(now);chromatic_here=1;chromatic_seen_us=now;}
}
/* B on the card's Home: back to the Chromatic, on the "On this Pocket" tile. The saved Chromatic library shows at
 * once (else the start screen) and the search asks now, so a connected Chromatic's Home follows within seconds. */
static void card_leave(uint32_t now) {
    static pj_library_item tile;snprintf(tile.title,sizeof(tile.title),"%s",PJ_CARD_TILE_TITLE);
    card_visit=card_folder_pending=0;library.count=library.row_count=0; /* no card title is taken for the Chromatic's focus */
    DLOG("card library left: back to the Chromatic");
    if(have_cache)connect_source(&cache_source,now);
    else{pj_live_session_close();disconnect();screen=PJ_SCREEN_SOURCE;set_status("");}
    focus_key=pj_sync_folder_key(&tile);focus_details=0;
    if(link_ok)search_now(now);
}
/* A search answer while the user is on the card by choice: noted, not taken. */
static void chromatic_present(uint32_t now) {
    if(!chromatic_here)DLOG("chromatic answers; the card library stays on screen");
    chromatic_here=1;chromatic_seen_us=now;
    link_source.close(link_source.context);search_open=0;search_at_us=now+PJ_TETHER_PING_US;
}
static void tether_search(uint32_t now) {
    if(tether!=TETHER_SEARCH||io==&link_source||playing||starting||want_start||restart)return;
    if(!search_open&&!search_armed) {
        if(!due(now,search_at_us,PJ_SEARCH_SPAN_US))return;
        if(!link_open()){tether=TETHER_OFF;return;}
        /* This step's rate, then the clock driven high and still for 4 ms: the Chromatic's
         * receiver arms on that, whatever it made of the lines while the link was closed. */
        if(search_step>=search_count)search_step=0;
        pj_hw_reset(&rpc.link);pj_ln.khz=pj_hw_rate(&rpc.link,search_list[search_step]);pj_hw_mode(&rpc.link,0);
        pj_ln.hold=1;search_armed=1;search_tries++;search_at_us=now+PJ_LINK_RESYNC_US;return;
    }
    if(search_armed) {
        if(!due(now,search_at_us,PJ_SEARCH_SPAN_US))return;
        search_armed=0;pj_ln.hold=0;pj_hw_mode(&rpc.link,1);
        if(!link_source.begin(link_source.context,PJ_RPC_HELLO,"{}",now)){link_source.close(link_source.context);search_at_us=now+PJ_TETHER_PING_US;return;}
        /* 2 s for the Chromatic to answer, plus the time the HELLO and a whole answer take on
         * the wire at this rate (0.7 s at 8 kHz). */
        search_open=1;search_at_us=now+PJ_TETHER_PING_US+pj_link_bytes_us(21u+512u);return;
    }
    int result=link_source.poll(link_source.context,now);
    if(result==PJ_POLL_READY){if(card_visit&&io==&stored_source)chromatic_present(now);else tether_found(now);return;}
    if(result!=PJ_POLL_WAITING||due(now,search_at_us,PJ_SEARCH_SPAN_US)) {
        /* No answer: the numbers go to the log (the first two rounds, then every 16th try), the
         * link closes, and the next try uses the next rate. A refusal or a link fault waits a
         * whole window instead. */
        if(search_tries<=2u*search_count||!(search_tries%16u))log_link(result==PJ_POLL_WAITING?"no answer":"search failed");
        link_source.close(link_source.context);search_open=0;
        search_step=(search_step+1u)%search_count;
        search_at_us=now+(result==PJ_POLL_WAITING?PJ_TETHER_GAP_US:PJ_TETHER_PING_US);
    }
}
/* While linked: no answer for 9 s to a request the Chromatic answers at once = unplugged.
 * Requests that wait for the server (Home, details, art, START) keep the 15 s deadline. */
static void tether_watch(uint32_t now) {
    if(tether!=TETHER_LINKED||io!=&link_source||!io_busy)return;
    /* A probe gets one second (plus the wire time); then straight back to the rate that works. */
    if(io_owner==OWNER_PROBE) {
        if(pj_ln.bad!=probe_bad)probe_failed(now,"probe answer damaged");
        else if(due(now,probe_deadline_us,PJ_SEARCH_SPAN_US))probe_failed(now,"probe not answered");
        return;
    }
    if(io_command!=PJ_RPC_HELLO&&io_command!=PJ_RPC_READ&&io_command!=PJ_RPC_STOP)return;
    /* Below 256 kHz a full answer takes longer on the wire: that time is added. */
    if((uint32_t)(now-tether_reply_us)>=PJ_TETHER_LOST_US+(pj_ln.khz<PJ_LINK_SAFE_KHZ?pj_link_bytes_us(1024u):0u))
        tether_lost(now,"no answer");
}
/* The header sign: a recent event, else the sync's progress, else the source badge. */
static void update_sign(uint32_t now) {
    const char *text="";int tone=PJ_SIGN_INFO;unsigned done=0,total=0;sign_hint=NULL;
    if(chromatic_here&&(uint32_t)(now-chromatic_seen_us)>=PJ_TETHER_LOST_US)chromatic_here=0;
    if(temp_until_us&&!due(now,temp_until_us,60000000u)){text=temp_sign;tone=temp_tone;}
    else {
        temp_until_us=0;
        if(tether==TETHER_LINKED&&io==&link_source&&tsync.state==PJ_SY_RUNNING) {
            done=tsync.done;total=tsync.total;tone=PJ_SIGN_BUSY;
            snprintf(sign_text,sizeof(sign_text),"Chromatic found: syncing %u of %u",done+1>total?total:done+1,total);text=sign_text;
        } else if(pj_store_busy(&store)&&store.image_pending){text="Saving for offline...";tone=PJ_SIGN_BUSY;}
        /* On the card's library by choice: the way back, and whether the Chromatic is still there. */
        else if(card_visit&&io==&stored_source) {
            text=chromatic_here?"Chromatic connected: B on Home goes back":"B on Home: back to your Chromatic";
            tone=chromatic_here?PJ_SIGN_DONE:PJ_SIGN_INFO;
        }
        else if(screen==PJ_SCREEN_SOURCE&&tether==TETHER_SEARCH) {
            text="Looking for your Chromatic";tone=PJ_SIGN_BUSY;sign_hint="Connect the link cable to the Chromatic, then open Media on it.";
        }
        /* The link numbers where the link state shows on the library screens: the search's
         * rate and bytes while it runs, and a link too slow to play. */
        else if(link_sign[0])text=link_sign;
    }
    if(tone==PJ_SIGN_WARN&&screen==PJ_SCREEN_SOURCE)sign_hint="Plug the cable back in to carry on; your place is kept.";
    if(strcmp(text,shown_sign)||done!=shown_done||total!=shown_total||tone!=sign_tone_now) {
        snprintf(shown_sign,sizeof(shown_sign),"%s",text);shown_done=done;shown_total=total;sign_tone_now=tone;
        progress_done=done;progress_total=total;redraw=1;
    }
}
/* Saving to the slots: a piece per pass, never while playing, never during a card read. */
static void store_tick(void) {
    if(!pj_store_busy(&store)||playing||starting||want_start||restart||!log_quiet())return;
    int had_image=store.image_pending;
    int result=pj_store_step(&store,&watch,clock_us);
    if(result<0)DLOG("cache save stopped: %s",store.why);
    if(result>0&&!pj_store_busy(&store)) {
        read_save_table("after state save");log_line();redraw=1;
        /* After B: the size the Pocket will write for the watch state, in the header for 6 s. */
        if(!had_image&&table_read) {
            char w[16],s[72];size_words(w,sizeof(w),&table_state);
            snprintf(s,sizeof(s),"Place saved: %s (the card gets it when you quit)",w);flash_sign(s,PJ_SIGN_DONE,10);
        }
    }
    if(!pj_store_busy(&store)&&had_image&&!store.image_pending) {
        DLOG("cache saved: %lu bytes, %u pieces so far, worst %lums",(unsigned long)store.card_size,
            store.pieces,(unsigned long)(store.worst_us/1000u));
        if(!store_reported)flash_sign("Saved for offline",PJ_SIGN_DONE,5);
        store_reported=1;
    }
}
static void play_selected(int from_start,uint32_t now) {
    pj_library_item *item=&library.items[selected];
    if(item->unavailable){set_status(io==&cache_source?"Connect the Chromatic to watch this":"Not on this Pocket");return;}
    if(link_slow()) {
        /* The library and pictures still work at this rate. Play is the user's word that a
         * faster one is wanted now: whatever holds the climb back (the ceiling an earlier failure
         * left, the wait before the next try) is dropped, the climb runs at once, and the title
         * starts if 256 kHz or better is reached; else the "too slow" words. B gives it up. */
        if(tether!=TETHER_LINKED){slow_status();return;}
        link_ceiling_khz=4000u;climb_retry_us=0;climb_wait_us=PJ_LINK_RETRY_CLIMB_US;
        play_pending=1;play_from_start=from_start;set_status("Trying a faster link...");return;
    }
    play_begin(from_start,now);
}
static void open_selected(uint32_t now) {
    pj_library_item *item=&library.items[selected];
    if(item->card_tile){card_open(now);return;}
    if(item->folder) {
        if(depth<8) {
            parents[depth]=parent;pages[depth]=page;from_home[depth]=(uint8_t)home;selected_at[depth++]=selected;
            parent=item->handle;page=0;selected=0;want_browse=1;
        } else set_status("Folder depth limit; B: back");
        return;
    }
    screen=PJ_SCREEN_DETAILS;action=0;details.valid=0;want_details=1;focus_us=now;redraw=1;
}
/* Up one folder. A folder opened from Home (such as "On this Pocket") goes back to
 * Home with the same tile selected, not to a browse of "no folder". */
static void leave_folder(void) {
    depth--;selected=selected_at[depth];
    if(from_home[depth]){want_home=1;want_browse=0;}
    else{parent=parents[depth];page=pages[depth];want_browse=1;}
}
static void go_back(uint32_t now) {
    if(screen==PJ_SCREEN_DETAILS){screen=home?PJ_SCREEN_HOME:PJ_SCREEN_LIBRARY;paused=0;focus_changed(now);return;}
    if(depth)leave_folder();
    else if(!home)want_home=1;
    else if(card_visit&&io==&stored_source)card_leave(now); /* the card's Home: back to the Chromatic */
}
static void move_home(int dx,int dy,uint32_t now) {
    unsigned focused=pj_ui_focused_row(&library,selected);
    const pj_library_row *row=&library.rows[focused];
    unsigned column=selected-row->first;
    if(dx) {
        if(dx<0&&column)selected--;
        else if(dx>0&&column+1<row->count)selected++;
    } else if(dy<0&&focused) {
        const pj_library_row *to=&library.rows[focused-1];
        selected=to->first+(column<to->count?column:to->count-1);
    } else if(dy>0&&focused+1<library.row_count) {
        const pj_library_row *to=&library.rows[focused+1];
        selected=to->first+(column<to->count?column:to->count-1);
    }
    focus_changed(now);
}
static void move_grid(int dx,int dy,uint32_t now) {
    unsigned columns=library.rows[0].shape==PJ_ART_THUMB?3u:4u;
    int to=(int)selected+dx+dy*(int)columns;
    /* Scrolling past the last row loads the next page and past the first row the
     * previous one, so a long folder reads as one list (R and L still jump pages). */
    if(dy>0&&to>=(int)library.count&&library.next<library.total) {
        page=library.next;selected=0;want_browse=1;focus_changed(now);return;
    }
    if(dy<0&&to<0&&page) {
        page=page>12?page-12:0;selected=0;want_browse=1;focus_changed(now);return;
    }
    if(to>=0&&to<(int)library.count)selected=(unsigned)to;
    focus_changed(now);
}
static void seek_by(int seconds) {
    if(playing)stop_at_position(1);
    else if(!paused){if(session)restart=1;else want_start=1;}
    if(seconds<0)position_seconds=position_seconds>(uint32_t)-seconds?position_seconds-(uint32_t)-seconds:0;
    else if(position_seconds<172789)position_seconds+=(uint32_t)seconds;
    if(library.count) {
        uint64_t duration=library.items[selected].duration_ticks/10000000;
        if(duration&&position_seconds>=duration)position_seconds=(uint32_t)(duration-1);
    }
    set_status(paused?"":"Seeking...");
}
static int handle_input(uint32_t now) {
    /* The About page sits on the start screen (offline): B or Y returns to it. */
    if(screen==PJ_SCREEN_CREDITS) {
        if(of_btn_pressed(OF_BTN_B)||of_btn_pressed(OF_BTN_Y)){screen=PJ_SCREEN_SOURCE;redraw=1;}
        return 0;
    }
    if(!online&&!io_busy) {
        if(of_btn_pressed(OF_BTN_Y)){screen=PJ_SCREEN_CREDITS;redraw=1;return 0;}
        /* B never leaves the app: from here it would drop the user into the OS menu with
         * no obvious way back. Quitting is the Pocket's own menu (Analogue button). */
        /* A fault reason stays on screen; B only explains how to quit when there is none. */
        if(of_btn_pressed(OF_BTN_B)){if(!status[0])set_status("To quit, use the Analogue button menu");return 0;}
        if(have_cache&&of_btn_pressed(OF_BTN_A))connect_source(&cache_source,now);
        /* The card's library chosen here stays on screen when the Chromatic answers (card_visit). */
        else if(have_stored&&(of_btn_pressed(OF_BTN_A)||(have_cache&&of_btn_pressed(OF_BTN_SELECT)))) {
            connect_source(&stored_source,now);card_visit=io==&stored_source;
        }
        else if(of_btn_pressed(OF_BTN_X)||(!have_stored&&!have_cache&&of_btn_pressed(OF_BTN_A)))search_now(now);
        return 0;
    }
    if(!online)return 0;
    if(of_btn_pressed(OF_BTN_SELECT)) {
        if(io&&io->stored&&!playing&&!starting&&!paused){
            disconnect();screen=PJ_SCREEN_SOURCE;set_status("");card_visit=0;return 0;
        }
        if(!io->stored) {
            if(playing)stop_at_position(1);
            if(starting){cancel_start=1;restart=1;}
            quality=(quality+1)%3;redraw=1;
        }
    }
    if(starting) {
        if(of_btn_pressed(OF_BTN_B)){cancel_start=1;restart=0;set_status("Cancelling playback...");}
        return 0;
    }
    if(play_pending) { /* the climb Play asked for is running: B gives it up, nothing else is taken */
        if(of_btn_pressed(OF_BTN_B)){play_pending=0;set_status("");}
        return 0;
    }
    if(playing||paused||want_stop||want_start) {
        /* L: playback numbers, then (from the Chromatic) the link numbers, then off. */
        if(of_btn_pressed(OF_BTN_L1)){hud=(hud+1)%(io==&link_source?3:2);osd_show(now);}
        if(of_btn_pressed(OF_BTN_R1)&&playing){pj_live_session_toggle_film();osd_show(now);}
        if(of_btn_pressed(OF_BTN_B)) {
            if(playing)stop_at_position(0);
            paused=0;want_start=restart=0;start_refusals=0;screen=PJ_SCREEN_DETAILS;action=0;set_status("");
        } else if(of_btn_pressed(OF_BTN_START)||(paused&&of_btn_pressed(OF_BTN_A))) {
            if(playing){stop_at_position(0);paused=1;screen=PJ_SCREEN_DETAILS;action=0;set_status("");}
            else if(paused){paused=0;if(session)restart=1;else want_start=1;set_status("Resuming...");}
        } else if(of_btn_pressed(OF_BTN_LEFT)||of_btn_pressed(OF_BTN_RIGHT)) {
            seek_by(of_btn_pressed(OF_BTN_LEFT)?-10:10);osd_show(now);redraw=1;
        }
        return 0;
    }
    /* A sync request in flight never holds up the user's buttons. */
    if(io_busy&&io_owner==OWNER_APP&&(io_command==PJ_RPC_HOME||io_command==PJ_RPC_BROWSE))return 0;
    /* Found, Home not yet asked (the faster rate is being tried first, by a still clock and then
     * a probe HELLO): the titles on screen are still the saved ones, so buttons wait like they
     * do for Home itself. The start screen's X and B while playing are handled above. Y and B
     * still work (also while a Home refused "Busy." waits its 0.7 s): neither opens a title, and
     * with Home on its way B only leaves Details (a folder left now would be browsed after it). */
    if(of_btn_pressed(OF_BTN_Y)){selected=0;want_home=1;screen=PJ_SCREEN_HOME;return 0;}
    if(of_btn_pressed(OF_BTN_B)){if(!(want_home&&io==&link_source)||screen==PJ_SCREEN_DETAILS)go_back(now);return 0;}
    if(want_home&&io==&link_source)return 0;
    if(!library.count)return 0;
    int dx=of_btn_pressed(OF_BTN_LEFT)?-1:of_btn_pressed(OF_BTN_RIGHT)?1:0;
    int dy=of_btn_pressed(OF_BTN_UP)?-1:of_btn_pressed(OF_BTN_DOWN)?1:0;
    if(screen==PJ_SCREEN_DETAILS&&countdown_item>=0) {
        if(of_btn_pressed(OF_BTN_B)){countdown_item=-1;redraw=1;}
        else if(of_btn_pressed(OF_BTN_A))countdown_until_us=now;
        return 0;
    }
    if(screen==PJ_SCREEN_DETAILS) {
        pj_library_item *item=&library.items[selected];
        int two=item->resume_ticks&&!item->played;
        if(two&&dx){action=dx>0;redraw=1;}
        if(of_btn_pressed(OF_BTN_A))play_selected(!two||action==1?two:0,now);
        if(of_btn_pressed(OF_BTN_X))play_selected(1,now);
        return 0;
    }
    if(dx||dy) {
        if(screen==PJ_SCREEN_HOME)move_home(dx,dy,now);
        else move_grid(dx,dy,now);
    }
    if(!home&&of_btn_pressed(OF_BTN_R1)&&library.next<library.total){page=library.next;selected=0;want_browse=1;}
    if(!home&&of_btn_pressed(OF_BTN_L1)&&page){page=page>12?page-12:0;selected=0;want_browse=1;}
    if(of_btn_pressed(OF_BTN_A))open_selected(now);
    else if(of_btn_pressed(OF_BTN_X)&&!library.items[selected].folder)play_selected(1,now);
    return 0;
}
/* Runs inside the video path, only while the strip is showing. */
static void draw_osd(uint16_t *strip) {
    pj_live_session_stats stats;pj_live_session_status(&stats);
    uint32_t position=base_seconds+(uint32_t)(stats.clock_pts/90000);
    uint32_t duration=library.count?(uint32_t)(library.items[selected].duration_ticks/10000000):0;
    char note[80]="";
    /* Scanout line rate is 31507.7 Hz; hundredths of a Hz keep this integer. */
    unsigned refresh=stats.refresh_lines?3150769u/stats.refresh_lines:0;
    if(hud==2) {
        char words[48];pj_link_fault_words(pj_ln.faults,words,sizeof(words));
        snprintf(note,sizeof(note),"Link %u kHz  got %lu B  %lu good  %lu bad  faults %s",pj_ln.khz,
            (unsigned long)(pj_ln.received%100000000u),(unsigned long)(pj_ln.good%1000000u),
            (unsigned long)(pj_ln.bad%1000000u),words);
    } else if(hud)snprintf(note,sizeof(note),"%u fps  panel %u.%02u Hz  shown %lu  drop %lu  gaps %lu",
        stats.fps,refresh/100,refresh%100,(unsigned long)stats.shown,
        (unsigned long)stats.dropped,(unsigned long)stats.audio_starves);
    pj_ui_osd(strip,library.count?library.items[selected].title:"",position,duration,note);
}
static int osd_active(uint32_t now) {
    if(osd_until_us&&due(now,osd_until_us,PJ_OSD_US))osd_until_us=0; /* 0: not showing */
    return hud||osd_until_us;
}
static void startup(void) {
    /* Everything a user would otherwise need a debugger for, on screen. */
    const struct of_capabilities *caps=of_get_caps();
    snprintf(diagnostics[0],sizeof(diagnostics[0]),"Runtime: %s",caps?"ok":"NOT FOUND");
    pj_live_selftest_result test;pj_live_session_selftest(&test);
    snprintf(diagnostics[1],sizeof(diagnostics[1]),"FPGA JPEG decoder: %s",test.message);
    /* Keep the previous run's newest lines first: if it froze, they are the only record. */
    pj_diag_resume(&diag);
    DLOG("start: runtime %s, decoder %s, card read limit %lu bytes",caps?"ok":"missing",test.message,
        (unsigned long)pj_sd_io_limit());
    /* What the Pocket loaded: the seed's 32768 means no earlier quit wrote the slot back. */
    read_save_table("at start");
    /* The saved Chromatic library and the watch state, before any card read starts. */
    uint32_t opened=of_time_us();
    cache_live=0;
    int image=pj_store_load(&store,&watch,&cache_images[0],cache_bytes[0],sizeof(cache_bytes[0]));
    have_cache=image&&pj_tc_find(saved_image,PJ_RPC_HOME,0,0,0)!=NULL;
    DLOG("saved state: %u watch records, library %s (%lu bytes, %u art) in %lums",watch.count,
        have_cache?"ok":store.why,(unsigned long)saved_image->used,pj_tc_count(saved_image,PJ_RPC_ART),
        (unsigned long)((of_time_us()-opened)/1000u));
    if(have_cache)snprintf(diagnostics[3],sizeof(diagnostics[3]),"Saved from your Chromatic: %u tiles",pj_tc_count(saved_image,PJ_RPC_ART));
    else diagnostics[3][0]=0;
    settings_start(); /* the Pocket menu's Core Settings, with the copy kept beside the watch state */
    opened=of_time_us();
    have_stored=pj_sd_open(&stored_source,&stored,clock_us);
    uint32_t took=of_time_us()-opened;
    DLOG("library open %s: %s, %lu records %lu titles, %lu bytes in %lu reads, %lums (%lu KB/s)",
        have_stored?"ok":"failed",have_stored?"":stored.why,(unsigned long)stored.record_count,
        (unsigned long)stored.title_count,(unsigned long)stored.io_bytes,(unsigned long)stored.io_chunks,
        (unsigned long)(took/1000u),(unsigned long)(took?(uint64_t)stored.io_bytes*1000u/took:0));
    if(have_stored)snprintf(diagnostics[2],sizeof(diagnostics[2]),"Library on card: %lu titles",(unsigned long)stored.title_count);
    else {
        stored_source.context=NULL;
        snprintf(diagnostics[2],sizeof(diagnostics[2]),"Library on card: %s",
            stored.why&&*stored.why?stored.why:"none - run PC sync");
    }
    have_link=1;link_source.context=&rpc;cache_source.context=&cache_reader;
    /* The link needs the paired media core; without it the search stays off. */
    link_ok=link_open();
    if(link_ok) {
        /* The slowest clock this core runs: 8 kHz, or 256 on a core from before the slow rates. */
        pj_ln.slowest_khz=pj_hw_slowest_khz(&rpc.link);
        link_source.close(link_source.context);
    }
    search_plan();
    tether=link_ok?TETHER_SEARCH:TETHER_OFF;search_at_us=of_time_us();
    if(!link_ok)snprintf(diagnostics[3],sizeof(diagnostics[3]),"Chromatic link: needs the paired media core");
    DLOG("chromatic link %s; slowest clock %u kHz%s",link_ok?"ready, searching":"unavailable",pj_ln.slowest_khz,
        link_ok&&pj_ln.slowest_khz>=PJ_LINK_SAFE_KHZ?" (older core: 256 kHz only)":"");
    save_log("start"); /* nothing is in flight after the library open */
    log_line();
    /* The same table line in the Home header for 12 s, so no button press is needed to see it
     * (the start screen keeps it: Select on Home returns there). */
    if(table_read) {
        char a[16],b[16],s[72];size_words(a,sizeof(a),&table_log);size_words(b,sizeof(b),&table_state);
        snprintf(s,sizeof(s),"Save sizes: log %s, place %s",a,b);flash_sign(s,PJ_SIGN_INFO,20);
    }
}
/* The end of a title: watched, back to Details, and for an episode the next one in the list
 * after a short countdown. */
static void finished(uint32_t now) {
    stop_at_position(0);note_position(0,1);paused=0;screen=PJ_SCREEN_DETAILS;action=0;
    set_status("Finished");
    DLOG("finished");log_pending="finished";
    countdown_item=-1;
    if(!library.count||selected+1>=library.count)return;
    const pj_library_item *now_item=&library.items[selected],*next=&library.items[selected+1];
    unsigned row=pj_ui_focused_row(&library,selected);
    int same_row=pj_ui_focused_row(&library,selected+1)==row;
    /* Home rows mix series: only the same series counts there. */
    if(now_item->episode&&next->episode&&!next->folder&&!next->unavailable&&same_row&&
       (!home||!strcmp(now_item->title,next->title))) {
        countdown_item=(int)(selected+1);countdown_until_us=now+PJ_NEXT_EPISODE_US;
        snprintf(countdown_text,sizeof(countdown_text),"Next episode in 5");set_status("");
    }
}
static void countdown_tick(uint32_t now) {
    if(countdown_item<0)return;
    if(screen!=PJ_SCREEN_DETAILS||!online||countdown_item>=(int)library.count){countdown_item=-1;redraw=1;return;}
    int32_t left=(int32_t)(countdown_until_us-now);
    if(left<=0) {
        selected=(unsigned)countdown_item;countdown_item=-1;details.valid=0;want_details=1;
        play_selected(0,now);redraw=1;return;
    }
    char text[40];snprintf(text,sizeof(text),"Next episode in %lu",(unsigned long)((left+999999)/1000000));
    if(strcmp(text,countdown_text)){snprintf(countdown_text,sizeof(countdown_text),"%s",text);redraw=1;}
}
/* One pass of the app loop. 1 to leave the app (never today). */
static int app_step(uint32_t now) {
    if(playing){uint32_t pass=now-loop_last_us;if(pass>loop_max_us)loop_max_us=pass;loop_passes++;}
    loop_last_us=now;
    of_input_poll();
    settings_poll(now);
    if(handle_input(now))return 1;
    if(playing) {
        if(pj_live_session_tick(now)<0){fault_playback("Playback failed");return 0;}
        if(chunk_size||stream.used==stream.target) {
            size_t consumed=0;
            int result=pj_stream_reader_feed(&stream,chunk+chunk_used,chunk_size-chunk_used,&consumed,pj_live_session_push);
            chunk_used+=consumed;
            if(result<0){fault_playback("Stream check failed");return 0;}
            if(chunk_used==chunk_size&&result>0)chunk_size=chunk_used=0;
        }
        pj_live_session_stats stats;pj_live_session_status(&stats);
        if(stats.failed){fault_playback("Decoder failed");return 0;}
        /* Still buffering 10 s after this stream's first data arrived (30 s with none at all): it
         * is not going to start. Words and Details instead of a frozen first picture. */
        if(stats.buffering&&(got_data?(uint32_t)(now-first_data_us)>PJ_BUFFER_LIMIT_US:
                                      (uint32_t)(now-play_started_us)>PJ_NO_DATA_LIMIT_US)) {
            DLOG("still buffering: %s",got_data?"10 s after the first data":"no data in 30 s");
            stop_at_position(0);paused=0;screen=PJ_SCREEN_DETAILS;action=0;
            set_status(got_data?"The stream did not start in 10 s":"No stream data in 30 s");
            return 0;
        }
        if(due(now,next_stats_us,1000000u)) {
            next_stats_us=now+1000000u;
            if((uint32_t)(now-play_started_us)>60000000u)read_resyncs=0;
            DLOG("t+%lus pts %lums shown %lu drop %lu gaps %lu refill %lu%s buf %d held %u.%us read %luKB/s %lu reads max %lums "
                "waiting %s %lums at %llu card %d busy %d loops %lu max %lums",
                (unsigned long)((now-play_started_us)/1000000u),(unsigned long)(stats.clock_pts/90u),
                (unsigned long)stats.shown,(unsigned long)stats.dropped,(unsigned long)stats.audio_starves,
                (unsigned long)stats.rebuffers,stats.rebuffering?"*":"",
                stats.buffering,stats.buffered_tenths/10u,stats.buffered_tenths%10u,(unsigned long)(read_bytes/1024u),(unsigned long)reads,
                (unsigned long)(read_max_us/1000u),io_busy&&io_command==PJ_RPC_READ?"read":io_busy?"other":"none",
                (unsigned long)(io_busy?(now-read_sent_us)/1000u:0),(unsigned long long)offset,
                io==&stored_source?(int)stored.state:-1,!pj_sd_io_quiet(),(unsigned long)loop_passes,
                (unsigned long)(loop_max_us/1000u));
            loop_passes=loop_max_us=0;
            read_bytes=reads=read_max_us=0;
            if(io==&link_source&&!(((now-play_started_us)/1000000u)%5u))log_link("playing");
        }
        /* Checkpoints so a freeze with no error still leaves a log: every 2 s for the
         * first 12 s (the stop seen on hardware is near 5 s), then every 15 s. */
        if(due(now,next_save_us,15000000u)) {
            next_save_us=now+((now-play_started_us)<12000000u?2000000u:15000000u);
            if(!log_pending)log_pending="checkpoint";
        }
        if(stats.complete&&stream_eof)finished(now);
    }
    if(io&&(!playing||!chunk_size)) {
        int result=io->poll(io->context,now);
        int owner=io_owner;
        if(result!=PJ_POLL_WAITING&&io==&link_source&&result!=PJ_POLL_FAILED)tether_reply_us=now;
        if(result==PJ_POLL_FAILED&&owner==OWNER_PROBE&&io==&link_source&&tether==TETHER_LINKED) {
            probe_failed(now,"probe failed");result=PJ_POLL_WAITING; /* the link stays up at the old rate */
        }
        /* A START not answered in 15 s. The Chromatic answers a START at once (it never waits for
         * the server), so the silence is the Chromatic's or the cable's. Say so, and prove the
         * cable with a HELLO at once: only if that too goes unanswered (4 s) is it "unplugged".
         * A late answer to the START is ignored. If only the answer was lost the Chromatic has a
         * stream open and would refuse every picture ("Busy playing."): the STOP that follows the
         * HELLO closes it (it closes whatever is open, the session number does not matter). */
        if(result==PJ_POLL_FAILED&&owner==OWNER_APP&&io==&link_source&&tether==TETHER_LINKED&&
           io_command==PJ_RPC_START&&rpc.waiting&&pj_rpc_expired(&rpc,now)) {
            DLOG("start not answered in 15 s");
            pj_rpc_cancel(&rpc);io_busy=0;starting=cancel_start=0;restart=want_start=0;start_refusals=0;paused=0;
            session=0;want_stop=1;
            set_status("No answer from the Chromatic: try again");
            tether_reply_us=now-(PJ_TETHER_LOST_US-4000000u);gate_begin(pj_ln.khz,OWNER_ALIVE,now);
            result=PJ_POLL_WAITING;
        }
        if(result==PJ_POLL_FAILED) {
            io_busy=0;
            if(io==&link_source&&tether==TETHER_LINKED){tether_lost(now,"request failed");return 0;}
            char message[80];
            if(io==&link_source)snprintf(message,sizeof(message),
                "No answer from the Chromatic. Open Media on it; this Pocket keeps looking.");
            else snprintf(message,sizeof(message),"Source lost: %s",
                stored.why&&*stored.why?stored.why:"no reply");
            fault(message);return 0;
        }
        if(result!=PJ_POLL_WAITING)io_owner=OWNER_APP;
        if(result!=PJ_POLL_WAITING&&owner==OWNER_PROBE) {
            /* Answered at the faster rate. It is kept only when 600 bytes of answers have
             * arrived with every checksum good; until then the next HELLO goes out at once. */
            if(result==PJ_POLL_READY){size_t n=0;const uint8_t *p=io->reply(io->context,&n);hello_again(p,n);}
            io_busy=0;result=PJ_POLL_WAITING;
            if(pj_ln.bad!=probe_bad)probe_failed(now,"probe answer damaged");
            else if(pj_ln.received-probe_got>=PJ_LINK_PROBE_BYTES) {
                found_bad=pj_ln.bad;found_faults=pj_ln.fault_events;climb_wait_us=PJ_LINK_RETRY_CLIMB_US;
                /* The rate that plays, once proven, is the floor again: after a search that
                 * found the Chromatic slower, three damaged frames here would otherwise step
                 * down to 128 kHz and stop the title. Below the play rate the quiet step down
                 * stays (a damaged frame costs a 3 s retry there and nothing plays anyway). */
                if(link_good_khz<PJ_LINK_PLAY_KHZ&&pj_ln.khz>=PJ_LINK_PLAY_KHZ)link_good_khz=PJ_LINK_PLAY_KHZ;
                log_link("faster rate proven");
            } else if(probe_asks<PJ_LINK_PROBE_ASKS&&request(PJ_RPC_HELLO,"{}",now)) {
                io_owner=OWNER_PROBE;probe_asks++;
                probe_deadline_us=now+PJ_LINK_PROBE_US+pj_link_bytes_us(21u+512u);
            } else probe_failed(now,"probe too short");
        }
        if(result!=PJ_POLL_WAITING&&owner==OWNER_ALIVE&&!(alive_count++%12u))log_link("alive");
        if(result==PJ_POLL_REFUSED) {
            io_busy=0;
            if(owner==OWNER_SYNC) {
                /* "Busy.": the same step goes out again after the wait (a failed step is sent
                 * again); any other refusal, or too many, skips it as before. */
                char why[96];refusal_reason(why,sizeof(why));
                pj_sync_reply(&tsync,lib_busy_again(why)?PJ_POLL_FAILED:PJ_POLL_REFUSED,NULL,0);
            }
            else if(owner==OWNER_APP)handle_refused(io_command);
        } else if(result==PJ_POLL_READY) {
            size_t n=0;const uint8_t *p=io->reply(io->context,&n);
            io_busy=0;
            if(owner==OWNER_SYNC||(owner==OWNER_APP&&(io_command==PJ_RPC_HOME||io_command==PJ_RPC_BROWSE||
               io_command==PJ_RPC_DETAILS||io_command==PJ_RPC_ART)))lib_reset();
            if(owner==OWNER_SYNC) {
                if(pj_sync_reply(&tsync,PJ_POLL_READY,p,n)==2&&tsync.art.total) {
                    pj_art_store(&art_cache,&tsync.art);redraw=1; /* the tile appears as it arrives */
                }
                redraw=1;
            } else if(owner==OWNER_ALIVE)hello_again(p,n);
            else if(owner==OWNER_APP&&!handle_reply(io_command,p,n)){fault("Invalid reply from source");return 0;}
        }
    }
    tether_search(now);
    tether_watch(now);
    climb_due(now);link_clean_tick(now);
    gate_tick(now);
    countdown_tick(now);
    /* Here a READ reply has been handled and the next is not yet submitted. */
    if(log_pending)save_log(log_pending);
    schedule(now);
    store_tick();
    link_text();
    update_sign(now);
    if(redraw&&!playing)show_ui();
    return 0;
}
int main(void) {
    if(!of_get_caps())return 1;
    of_video_init();of_video_set_color_mode(OF_VIDEO_MODE_RGB565);
    of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
    set_status("Starting...");show_ui();
    startup();set_status(test_passed()?"":"Decoder self-test failed; see above");
    pj_live_session_set_osd(draw_osd,osd_active);
    /* A healthy card goes straight to Home, like opening the app: the saved Chromatic library
     * first, else the PC library. The search for the Chromatic runs in the background. */
    if(test_passed()) {
        if(have_cache)connect_source(&cache_source,of_time_us());
        else if(have_stored)connect_source(&stored_source,of_time_us());
    }
    update_sign(of_time_us());show_ui();
    for(;;) {
        if(app_step(of_time_us()))return 0;
        /* Sleeping here would cap decoding at a few frames per second. */
        if(!playing)usleep(1000);
    }
}
