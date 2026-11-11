/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Includes the actual app/controller; substitutes only SDK IO and physical link.
 * Also renders every native screen from synthetic titles and art for review.
 */
#define OF_PC 1
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define PJ_POCKET_MEDIA_LINK_H
#define PJ_LINK_HW_HOST 1
typedef struct {int available,tx_active;uint8_t *rx;} pj_pocket_link;
static unsigned link_sends;
static uint8_t injected[2048];
static size_t injected_size;
#define PJ_LINK_BASE 0x4d000000u
static int pj_link_open(pj_pocket_link *l,volatile uint32_t *regs,uint8_t *rx,size_t size) {
    (void)regs;(void)size;l->rx=rx;l->available=1;l->tx_active=0;return 1;
}
static void pj_link_close(pj_pocket_link *l){l->available=l->tx_active=0;}
static int pj_link_send(pj_pocket_link *l,const uint8_t *p,size_t n) {
    (void)p;(void)n;if(!l->available||l->tx_active)return 0;l->tx_active=1;link_sends++;return 1;
}
static void pj_link_pump_tx(pj_pocket_link *l,unsigned budget){(void)budget;l->tx_active=0;}
static int pj_link_poll(pj_pocket_link *l,unsigned budget,size_t *n) {
    (void)budget;
    if(injected_size){memcpy(l->rx,injected,injected_size);*n=injected_size;injected_size=0;return 1;}
    return 0;
}
int pj_sd_pc_read(unsigned slot,uint32_t offset,void *dest,uint32_t size) {
    (void)slot;(void)offset;(void)dest;(void)size;return -1; /* no card in this harness */
}
#define main pj_unexecuted_device_main
#include "live_player.c"
#undef main
/* The link block of link_hw.h: every rate from 8 kHz, no faults. The search, the rate climb and
 * the fallbacks are checked by the tether simulation in the Jellymatic repository
 * (https://github.com/ChaoticSPARTAN1/Jellymatic). */
static unsigned link_khz=256,link_resets;
static unsigned pj_hw_slowest_khz(pj_pocket_link *l){(void)l;return 8;}
static unsigned pj_hw_rate(pj_pocket_link *l,unsigned khz){(void)l;link_khz=khz;return khz;}
static unsigned pj_hw_take_faults(pj_pocket_link *l){(void)l;return 0;}
static void pj_hw_reset(pj_pocket_link *l){l->tx_active=0;link_khz=256;link_resets++;pj_ln.ack_ok=0;}
static void pj_hw_mode(pj_pocket_link *l,int polling){(void)l;(void)polling;}
static int pj_hw_quiet(pj_pocket_link *l){return !l->tx_active;}
static unsigned link_fills;
static int pj_hw_fill(pj_pocket_link *l){if(l->tx_active||pj_ln.hold)return 0;link_fills++;return 1;}
static int pj_hw_ack(pj_pocket_link *l,const uint8_t *w,unsigned n){(void)w;(void)n;if(l->tx_active||pj_ln.hold)return 0;link_fills++;return 1;}
#include <stdlib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"live controller failed at %d\n",__LINE__);exit(1);}} while(0)
static uint32_t buttons;
static unsigned opens,closes;
static pj_live_session_stats model_stats;
static uint8_t test_surface[640*240];
const struct of_capabilities *of_get_caps(void){return NULL;}
void of_video_init(void){}
uint8_t *of_video_surface(void){return test_surface;}
void of_video_flip(void){}
void of_video_set_display_mode(int mode){(void)mode;}
void of_video_set_color_mode(int mode){(void)mode;}
void of_video_get_mode(of_video_mode_t *mode) {
    memset(mode,0,sizeof(*mode));mode->width=320;mode->height=240;mode->stride=640;
    mode->color_mode=OF_VIDEO_MODE_RGB565;
}
unsigned int of_time_us(void){return 0;}
void of_input_poll(void){}
int of_btn_pressed(uint32_t mask){return (buttons&mask)!=0;}
int pj_live_session_open(void){opens++;return 1;}
void pj_live_session_close(void){closes++;}
void pj_live_session_status(pj_live_session_stats *s){*s=model_stats;}
int pj_live_session_tick(uint32_t now){(void)now;return 0;}
int pj_live_session_push(const uint8_t *p,size_t n){(void)p;(void)n;return 1;}
void pj_live_session_selftest(pj_live_selftest_result *r){r->passed=1;strcpy(r->message,"ok, test frame 3120 us");}
int pj_live_session_selftest_passed(void){return 1;}
void pj_live_session_set_osd(void (*draw)(uint16_t *),int (*active)(uint32_t)){(void)draw;(void)active;}
int pj_live_session_toggle_film(void){return 1;}
static unsigned set_buffer=9,set_gain=9,set_calls;
void pj_live_session_set_options(unsigned buffer,unsigned gain){set_buffer=buffer;set_gain=gain;set_calls++;}
static void press(uint32_t mask){buttons=mask;CHECK(!handle_input(100));buttons=0;}
static void reply(unsigned command,const char *json) {
    io_busy=0;rpc.waiting=0;rpc.link.tx_active=0;
    CHECK(handle_reply(command,(const uint8_t *)json,strlen(json)));
}
static void request_check(unsigned command,uint64_t seek,const char *profile) {
    schedule(200);CHECK(io_busy&&io_command==command);
    pj_json j;pj_json_token tokens[64];uint64_t value;char text[80];
    CHECK(pj_json_parse(&j,rpc.request+16,rpc.request_size-16,tokens,64));
    if(command==PJ_RPC_START) {
        CHECK(pj_json_u64(&j,pj_json_find(&j,0,"start_seconds"),&value)&&value==seek);
        CHECK(pj_json_text(&j,pj_json_find(&j,0,"profile"),text,sizeof(text))&&!strcmp(text,profile));
    }
}
static void inject(unsigned command,uint32_t sequence,unsigned flags) {
    injected_size=pj_rpc_encode(injected,sizeof(injected),command,sequence,"{}",2);
    injected[5]=(uint8_t)flags;uint32_t crc=pj_av_crc_part(0xffffffffu,injected,12);
    pj_put32(injected+12,pj_av_crc_part(crc,injected+16,2)^0xffffffffu);
}
static void rpc_checks(void) {
    pj_rpc_client c;CHECK(pj_rpc_open(&c,NULL));uint32_t start=0xffffff00u;
    CHECK(pj_rpc_begin(&c,PJ_RPC_HOME,"{}",start));
    CHECK(!pj_rpc_begin(&c,PJ_RPC_HOME,"{}",start));
    uint8_t copy[32];memcpy(copy,c.request,c.request_size);unsigned before=link_sends;
    /* A timed resend goes out behind a still clock (polling off, wire empty, then 4 ms), the
     * same re-sync a HELLO gets: the peer may have lost its byte alignment with the request. */
    CHECK(!pj_rpc_poll(&c,start+3100000u)&&c.resync==1&&c.retries==0&&link_sends==before);
    CHECK(!pj_rpc_poll(&c,start+3100001u)&&c.resync==2&&link_sends==before);
    CHECK(!pj_rpc_poll(&c,start+3100000u+PJ_LINK_RESYNC_US)&&c.resync==2&&c.retries==0&&link_sends==before);
    CHECK(!pj_rpc_poll(&c,start+3100001u+PJ_LINK_RESYNC_US)&&!c.resync&&c.retries==1&&link_sends==before+1);
    CHECK(!memcmp(copy,c.request,c.request_size));
    inject(PJ_RPC_HOME,c.sequence+1,1);CHECK(!pj_rpc_poll(&c,start+3200000u)&&c.waiting);
    inject(PJ_RPC_HOME,c.sequence,1);CHECK(pj_rpc_poll(&c,start+3300000u)==1&&!c.waiting);
    CHECK(pj_rpc_begin(&c,PJ_RPC_HOME,"{}",100));
    inject(PJ_RPC_HOME,c.sequence,3);CHECK(pj_rpc_poll(&c,110)==-2&&!c.waiting);
    CHECK(pj_rpc_begin(&c,PJ_RPC_HOME,"{}",200));
    /* A damaged reply is counted and the request is sent again at once, three times; the
     * fourth damaged reply fails the request. A good reply after a damaged one is taken. */
    uint32_t bad=pj_ln.bad;before=link_sends;
    for(unsigned i=1;i<=3;i++) {
        inject(PJ_RPC_HOME,c.sequence,1);injected[17]^=1;
        CHECK(pj_rpc_poll(&c,210+i)==0&&c.waiting&&c.retries==i&&pj_ln.bad==bad+i&&link_sends==before+i);
    }
    inject(PJ_RPC_HOME,c.sequence,1);injected[17]^=1;CHECK(pj_rpc_poll(&c,220)==-1&&pj_ln.bad==bad+4);
    c.waiting=0;c.link.tx_active=0;
    CHECK(pj_rpc_begin(&c,PJ_RPC_HOME,"{}",300));
    inject(PJ_RPC_HOME,c.sequence,1);injected[17]^=1;CHECK(pj_rpc_poll(&c,310)==0&&c.retries==1);
    uint32_t good=pj_ln.good;
    inject(PJ_RPC_HOME,c.sequence,1);CHECK(pj_rpc_poll(&c,320)==1&&!c.waiting&&pj_ln.good==good+1);
    c.waiting=0;c.link.tx_active=0;
    CHECK(pj_rpc_begin(&c,PJ_RPC_HOME,"{}",start));CHECK(pj_rpc_poll(&c,start+15000001u)==-1);
}
/* Clearly synthetic pictures: soft gradients and circles, no real artwork. */
static uint16_t synthetic_pixel(unsigned seed,unsigned x,unsigned y,unsigned w,unsigned h) {
    static const unsigned from[6]={0x0d2b45,0x45210d,0x123d2a,0x3a1247,0x4a3a0a,0x0a3a4a};
    static const unsigned to[6]={0xe08a3c,0x48c0d8,0xd8c048,0xe05a7a,0x5ad0a0,0xc070e0};
    unsigned a=from[seed%6],b=to[seed%6];
    int cx=(int)(w*(3+seed%4)/8),cy=(int)(h*(2+seed%3)/6),dx=(int)x-cx,dy=(int)y-cy,r=(int)(h/3);
    unsigned t=y*255/h,glow=dx*dx+dy*dy<r*r?255u-(unsigned)((dx*dx+dy*dy)*200/(r*r)):0u;
    if(glow>t)t=glow;
    unsigned red=(((a>>16)&255)*(255-t)+((b>>16)&255)*t)/255;
    unsigned green=(((a>>8)&255)*(255-t)+((b>>8)&255)*t)/255;
    unsigned blue=((a&255)*(255-t)+(b&255)*t)/255;
    return pj_ui_rgb((red<<16)|(green<<8)|blue);
}
static void store_synthetic(uint32_t handle,unsigned kind) {
    pj_art_begin(&art,handle,kind);
    unsigned w=pj_art_width(kind),h=pj_art_height(kind);
    uint16_t *p=(uint16_t *)art.bytes;
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)p[y*w+x]=synthetic_pixel(handle,x,y,w,h);
    art.total=art.used=w*h*2;art.complete=1;pj_art_store(&art_cache,&art);art.handle=0;
}
static void write_screen(const char *folder,const char *name) {
    pj_ui_state s;fill_ui(&s);s.source="On this Pocket";s.quality="";s.can_stored=1;s.can_tether=1;
    pj_ui_draw(frame,&s);
    char path[512];snprintf(path,sizeof(path),"%s/%s.ppm",folder,name);
    FILE *f=fopen(path,"wb");CHECK(f);fprintf(f,"P6\n320 240\n255\n");
    for(unsigned i=0;i<320*240;i++) {
        unsigned c=frame[i];fputc((int)(((c>>11)&31)*255/31),f);fputc((int)(((c>>5)&63)*255/63),f);fputc((int)((c&31)*255/31),f);
    }
    CHECK(!ferror(f));fclose(f);
}
static void render_previews(const char *folder) {
    static const char sample[]="{\"rows\":["
    "{\"title\":\"Continue Watching\",\"shape\":0,\"items\":["
    "{\"handle\":11,\"title\":\"Sample Movie\",\"year\":2021,\"rating\":\"PG-13\",\"duration_ticks\":72000000000,\"resume_ticks\":28000000000,\"folder\":false,\"played\":false},"
    "{\"handle\":12,\"title\":\"Sample Episode\",\"subtitle\":\"Demo Series - S2:E5\",\"duration_ticks\":27000000000,\"resume_ticks\":9000000000,\"folder\":false,\"played\":false},"
    "{\"handle\":13,\"title\":\"Example Film\",\"year\":2019,\"duration_ticks\":61000000000,\"resume_ticks\":44000000000,\"folder\":false,\"played\":false},"
    "{\"handle\":14,\"title\":\"Demo Movie\",\"year\":2023,\"duration_ticks\":58000000000,\"resume_ticks\":5000000000,\"folder\":false,\"played\":false}]},"
    "{\"title\":\"Latest in Movies\",\"shape\":1,\"items\":["
    "{\"handle\":21,\"title\":\"Sample Film Two\",\"year\":2024,\"rating\":\"PG\",\"duration_ticks\":66000000000,\"resume_ticks\":0,\"folder\":false,\"played\":false},"
    "{\"handle\":22,\"title\":\"Test Movie\",\"year\":2022,\"duration_ticks\":70000000000,\"resume_ticks\":0,\"folder\":false,\"played\":true},"
    "{\"handle\":23,\"title\":\"Demo Clip\",\"year\":2023,\"duration_ticks\":59000000000,\"resume_ticks\":0,\"folder\":false,\"played\":false},"
    "{\"handle\":24,\"title\":\"Sample Film Three\",\"year\":2020,\"duration_ticks\":80000000000,\"resume_ticks\":0,\"folder\":false,\"played\":false,\"unavailable\":true},"
    "{\"handle\":25,\"title\":\"Example Movie\",\"year\":2021,\"duration_ticks\":64000000000,\"resume_ticks\":0,\"folder\":false,\"played\":false}]},"
    "{\"title\":\"My Media\",\"shape\":0,\"items\":["
    "{\"handle\":31,\"title\":\"Movies\",\"duration_ticks\":0,\"resume_ticks\":0,\"folder\":true,\"played\":false},"
    "{\"handle\":32,\"title\":\"Shows\",\"duration_ticks\":0,\"resume_ticks\":0,\"folder\":true,\"played\":false}]}],\"truncated\":false}";
    CHECK(pj_library_load(&library,(const uint8_t *)sample,strlen(sample),1));
    memset(&art_cache,0,sizeof(art_cache));
    for(unsigned i=0;i<library.count;i++) {
        for(unsigned kind=0;kind<3;kind++)store_synthetic(library.items[i].handle,kind);
    }
    home=1;screen=PJ_SCREEN_HOME;selected=0;paused=0;set_status("");
    store_synthetic(library.items[0].handle,PJ_ART_BACKDROP); /* the cache only keeps four heroes */
    write_screen(folder,"ui-home");
    selected=library.rows[1].first+1;store_synthetic(library.items[selected].handle,PJ_ART_BACKDROP);
    write_screen(folder,"ui-home-posters");
    selected=0;screen=PJ_SCREEN_DETAILS;action=0;store_synthetic(library.items[0].handle,PJ_ART_BACKDROP);
    static const char overview[]="{\"overview\":\"A synthetic preview entry. A retired navigator takes one last commission across a northern sea, and finds the charts have changed since anyone last dared the crossing. Placeholder text for layout review only.\",\"genres\":[\"Adventure\",\"Drama\"]}";
    CHECK(pj_details_load(&details,library.tokens,library.items[0].handle,(const uint8_t *)overview,strlen(overview)));
    write_screen(folder,"ui-details");
    selected=library.rows[1].first+3;action=0;write_screen(folder,"ui-details-not-on-card");
    static const char grid[]="{\"shape\":1,\"next\":8,\"total\":8,\"items\":["
    "{\"handle\":21,\"title\":\"Sample Film Two\",\"duration_ticks\":1,\"resume_ticks\":0,\"folder\":false,\"played\":false},"
    "{\"handle\":22,\"title\":\"Test Movie\",\"duration_ticks\":1,\"resume_ticks\":0,\"folder\":false,\"played\":true},"
    "{\"handle\":23,\"title\":\"Demo Clip\",\"duration_ticks\":1,\"resume_ticks\":0,\"folder\":false,\"played\":false},"
    "{\"handle\":25,\"title\":\"Example Movie\",\"duration_ticks\":1,\"resume_ticks\":0,\"folder\":false,\"played\":false},"
    "{\"handle\":11,\"title\":\"Sample Movie\",\"duration_ticks\":10,\"resume_ticks\":4,\"folder\":false,\"played\":false},"
    "{\"handle\":13,\"title\":\"Example Film\",\"duration_ticks\":1,\"resume_ticks\":0,\"folder\":false,\"played\":false},"
    "{\"handle\":14,\"title\":\"Demo Movie\",\"duration_ticks\":1,\"resume_ticks\":0,\"folder\":false,\"played\":false},"
    "{\"handle\":12,\"title\":\"Sample Episode\",\"duration_ticks\":1,\"resume_ticks\":0,\"folder\":false,\"played\":false}]}";
    CHECK(pj_library_load(&library,(const uint8_t *)grid,strlen(grid),0));
    for(unsigned i=0;i<library.count;i++)store_synthetic(library.items[i].handle,PJ_ART_POSTER);
    home=0;screen=PJ_SCREEN_LIBRARY;selected=1;write_screen(folder,"ui-library");
    screen=PJ_SCREEN_SOURCE;
    strcpy(diagnostics[0],"Runtime: ok");strcpy(diagnostics[1],"FPGA JPEG decoder: ok, test frame 3120 us");
    strcpy(diagnostics[2],"Library on card: 12 titles");strcpy(diagnostics[3],"Bridge: press X to try");
    set_status("");write_screen(folder,"ui-source");
    screen=PJ_SCREEN_CREDITS;write_screen(folder,"ui-credits");
    static uint16_t strip[PJ_FB_W*PJ_OSD_H];
    pj_ui_osd(strip,"Sample Movie",2812,7200,"");
    memset(frame,0,sizeof(frame));memcpy(frame+(PJ_FB_H-PJ_OSD_H)*PJ_FB_W,strip,sizeof(strip));
    for(unsigned y=30;y<210;y++)for(unsigned x=0;x<320;x++)frame[y*320+x]=synthetic_pixel(11,x,y-30,320,180);
    char path[512];snprintf(path,sizeof(path),"%s/ui-playback.ppm",folder);
    FILE *f=fopen(path,"wb");CHECK(f);fprintf(f,"P6\n320 240\n255\n");
    for(unsigned i=0;i<320*240;i++) {
        unsigned c=frame[i];fputc((int)(((c>>11)&31)*255/31),f);fputc((int)(((c>>5)&63)*255/63),f);fputc((int)((c&31)*255/31),f);
    }
    CHECK(!ferror(f));fclose(f);
}
/* ---- the review findings of 2026-11-09 (F1 to F8). Every case here fails on the code before. ---- */
static void refuse(unsigned command,const char *why) {
    char json[160];int n=snprintf(json,sizeof(json),"{\"error\":\"%s\"}",why);
    memcpy(rpc.response+PJ_RPC_HEADER,json,(size_t)n);rpc.response_size=PJ_RPC_HEADER+(size_t)n;
    io_busy=0;rpc.waiting=0;rpc.link.tx_active=0;handle_refused(command);
}
static void settle(void) {
    io_busy=0;rpc.waiting=0;rpc.link.tx_active=0;gate=0;pj_ln.hold=0;want_details=0;art.handle=0;
}
static void play_title(uint32_t id) {
    settle();selected=0;screen=PJ_SCREEN_DETAILS;play_begin(1,200);
    schedule(200);CHECK(io_busy&&io_command==PJ_RPC_START&&starting);
    char json[48];snprintf(json,sizeof(json),"{\"session\":%lu}",(unsigned long)id);
    reply(PJ_RPC_START,json);CHECK(playing&&session==id);
}
static void stop_answered(void) {
    settle();schedule(300);CHECK(io_busy&&io_command==PJ_RPC_STOP);
    reply(PJ_RPC_STOP,"{\"stopped\":true}");CHECK(!session&&!want_stop);
}
static void link_fix_checks(const uint8_t *home_data,size_t home_size) {
    found_bad=pj_ln.bad;found_faults=pj_ln.fault_events;model_stats.clock_pts=0;
    unsigned slowest=pj_ln.slowest_khz;pj_ln.slowest_khz=8; /* the core with the slow search rates */
    /* F1. A refused READ ends the title with the Chromatic's reason on Details. Before, the
     * READ was asked again at once, for ever, with nothing on screen. */
    play_title(50);
    settle();schedule(300);CHECK(io_busy&&io_command==PJ_RPC_READ);
    refuse(PJ_RPC_READ,"the stream is not an AVI");
    CHECK(!playing&&!paused&&!restart&&want_stop&&screen==PJ_SCREEN_DETAILS&&strstr(status,"the stream is not an AVI"));
    stop_answered();
    settle();schedule(400);CHECK(!(io_busy&&(io_command==PJ_RPC_READ||io_command==PJ_RPC_START)));
    /* "Read out of step." once: the stream starts again at the place on screen. Twice: it stops
     * with the words. */
    play_title(51);model_stats.clock_pts=3*90000;
    refuse(PJ_RPC_READ,"Read out of step.");
    CHECK(!playing&&want_stop&&restart&&read_resyncs==1&&position_seconds==3&&strstr(status,"same place"));
    settle();schedule(300);CHECK(io_busy&&io_command==PJ_RPC_STOP);
    reply(PJ_RPC_STOP,"{\"stopped\":true}");CHECK(want_start&&!restart);
    settle();request_check(PJ_RPC_START,3,"pocket-link-512-60");
    reply(PJ_RPC_START,"{\"session\":52}");CHECK(playing&&read_resyncs==1);
    refuse(PJ_RPC_READ,"Read out of step.");
    CHECK(!playing&&!restart&&want_stop&&screen==PJ_SCREEN_DETAILS&&strstr(status,"Read out of step"));
    stop_answered();model_stats.clock_pts=0;
    /* A refused STOP is "nothing left to stop": never asked again (before: for ever). */
    session=77;want_stop=1;settle();schedule(300);CHECK(io_busy&&io_command==PJ_RPC_STOP);
    refuse(PJ_RPC_STOP,"No such stream.");CHECK(!want_stop&&!session);
    session=78;want_stop=1;restart=1;refuse(PJ_RPC_STOP,"No such stream.");
    CHECK(!want_stop&&!session&&want_start&&!restart);want_start=0;
    /* F8. A refusal that waiting cannot clear is shown at once (before: asked six more times). */
    settle();selected=0;screen=PJ_SCREEN_DETAILS;play_begin(1,200);schedule(200);CHECK(starting);
    refuse(PJ_RPC_START,"Unknown media handle. Refresh the library.");
    CHECK(!starting&&!want_start&&!start_refusals&&
        (strstr(status,"Unknown media handle")||strstr(status,"could not start")));
    /* "Busy." (a request the Chromatic gave up on is still out at its server) is asked again
     * after 0.7 s, "Starting..." stays. "fast connection failed" lasts for the Chromatic's whole
     * link session (its launcher never gets the fast rate back inside one), so it is shown at
     * once, also after a "Busy." (before: asked again six times, which only delayed the words). */
    play_begin(1,200);settle();schedule(200);CHECK(starting);
    refuse(PJ_RPC_START,"Busy.");
    CHECK(!starting&&want_start&&start_refusals==1&&start_hold_us&&!strncmp(status,"Starting",8));
    settle();schedule(200);CHECK(!(io_busy&&io_command==PJ_RPC_START)&&want_start);
    settle();schedule(700001u);CHECK(io_busy&&io_command==PJ_RPC_START&&starting);
    refuse(PJ_RPC_START,"The link is too slow to play (fast connection failed).");
    CHECK(!starting&&!want_start&&!start_refusals&&
        (strstr(status,"fast connection failed")||!strcmp(status,"The Chromatic could not start this title")));
    settle();schedule(1500000u);CHECK(!(io_busy&&io_command==PJ_RPC_START)&&!want_start);
    play_begin(1,200);settle();schedule(200);CHECK(starting);
    refuse(PJ_RPC_START,"The link is too slow to play (fast connection failed).");
    CHECK(!starting&&!want_start&&!start_refusals&&strncmp(status,"Starting",8));
    want_start=0;start_refusals=0;start_hold_us=0;set_status("");
    /* A library request refused "Busy." is asked again 0.7 s later, six more times at most, then
     * the Chromatic's words; only the library waits meanwhile. Details, a folder page, a picture
     * (before: given up at once; a picture was stored blank). */
    settle();screen=PJ_SCREEN_DETAILS;selected=0;want_details=1;schedule(200);CHECK(io_busy&&io_command==PJ_RPC_DETAILS);
    for(unsigned i=1;i<=PJ_START_RETRIES;i++) {
        refuse(PJ_RPC_DETAILS,"Busy.");CHECK(want_details&&lib_refusals==i&&lib_hold_us);
        io_busy=0;rpc.waiting=0;rpc.link.tx_active=0;gate=0;pj_ln.hold=0;
        schedule(300);CHECK(!io_busy&&want_details);                      /* not before the 0.7 s */
        schedule(700001u);CHECK(io_busy&&io_command==PJ_RPC_DETAILS&&!want_details);
    }
    refuse(PJ_RPC_DETAILS,"Busy.");CHECK(!want_details&&!lib_refusals);   /* the seventh stands */
    settle();want_browse=1;parent=7;page=0;schedule(200);CHECK(io_busy&&io_command==PJ_RPC_BROWSE);
    refuse(PJ_RPC_BROWSE,"Busy.");CHECK(want_browse&&lib_refusals==1);
    io_busy=0;rpc.waiting=0;rpc.link.tx_active=0;schedule(700001u);CHECK(io_busy&&io_command==PJ_RPC_BROWSE);
    refuse(PJ_RPC_BROWSE,"No such folder.");CHECK(!want_browse&&!lib_refusals&&strstr(status,"No such folder"));
    settle();set_status("");screen=PJ_SCREEN_DETAILS;reset_art();
    pj_art_begin(&art,library.items[0].handle,PJ_ART_POSTER);schedule(200);CHECK(io_busy&&io_command==PJ_RPC_ART);
    refuse(PJ_RPC_ART,"Busy.");CHECK(art.handle&&lib_refusals==1&&!pj_art_find_card(&art_cache,library.items[0].handle,PJ_ART_POSTER));
    io_busy=0;rpc.waiting=0;rpc.link.tx_active=0;schedule(700001u);CHECK(io_busy&&io_command==PJ_RPC_ART);
    lib_reset();settle();
    /* F5. From "found" until Home is asked the titles on screen are the old ones: buttons wait
     * (before: only while the still clock ran, so A opened a title from the old list). */
    settle();screen=PJ_SCREEN_HOME;selected=1;want_home=1;want_browse=0;
    press(OF_BTN_A);CHECK(!want_browse&&selected==1&&screen==PJ_SCREEN_HOME&&depth==0);
    press(OF_BTN_RIGHT);press(OF_BTN_DOWN);press(OF_BTN_LEFT);CHECK(selected==1);
    want_home=0;
    /* F6. Times in the 32-bit clock: due unless 1..span ahead; one left over from long ago is due. */
    CHECK(due(100,100,50)&&due(100,99,50)&&!due(100,101,50)&&!due(100,150,50)&&due(100,151,50));
    CHECK(!due(0xfffffff0u,5,100)&&due(5,0xfffffff0u,100));
    CHECK(due(1000u+0x80000010u,1000,PJ_SEARCH_SPAN_US));
    hud=0;osd_until_us=1000;CHECK(osd_active(500)&&osd_until_us==1000);
    CHECK(!osd_active(1000u+0x80000010u)&&!osd_until_us); /* before: on screen for 35 minutes */
    settle();pj_ln.khz=64;link_good_khz=64;link_ceiling_khz=64;link_want_khz=512;climb_retry_us=5000000u;
    schedule(2000);CHECK(climb_retry_us==5000000u&&link_ceiling_khz==64&&!gate);
    settle();schedule(5000000u+0x90000000u); /* before: held for another 30 minutes */
    CHECK(!climb_retry_us&&link_ceiling_khz==4000u&&gate==1&&gate_khz==128&&gate_owner==OWNER_PROBE);
    /* F3 (b). Demoted after a loss with damage counted: the first try back up is 10 s later, and
     * the doubling wait is left alone (before: a minute, doubled each time). */
    settle();pj_ln.khz=256;link_good_khz=256;link_ceiling_khz=4000;climb_wait_us=PJ_LINK_RETRY_CLIMB_US;climb_retry_us=0;
    link_demote(1000);
    CHECK(link_good_khz==64&&link_ceiling_khz==64&&climb_retry_us==1000u+10000000u&&climb_wait_us==PJ_LINK_RETRY_CLIMB_US);
    pj_ln.khz=512;link_good_khz=256;link_ceiling_khz=4000;link_demote(2000);
    CHECK(link_good_khz==256&&link_ceiling_khz==256&&climb_retry_us==2000u+10000000u&&climb_wait_us==PJ_LINK_RETRY_CLIMB_US);
    /* F3 (c). Play on a link too slow to play drops the ceiling and the wait and climbs at once
     * (before: "too slow" and nothing else, for up to 16 minutes). B gives it up. */
    pj_ln.khz=64;link_good_khz=64;link_ceiling_khz=64;climb_retry_us=900000000u;climb_wait_us=8u*PJ_LINK_RETRY_CLIMB_US;
    selected=0;screen=PJ_SCREEN_DETAILS;play_selected(1,3000);
    CHECK(play_pending&&!want_start&&link_ceiling_khz==4000u&&!climb_retry_us&&
        climb_wait_us==PJ_LINK_RETRY_CLIMB_US&&strstr(status,"faster link"));
    settle();schedule(3000);CHECK(gate==1&&gate_khz==128&&gate_owner==OWNER_PROBE&&play_pending);
    press(OF_BTN_A);CHECK(play_pending&&!want_start);
    press(OF_BTN_B);CHECK(!play_pending&&screen==PJ_SCREEN_DETAILS);
    /* the climb is over and the link is still too slow: the words */
    settle();link_ceiling_khz=64;play_pending=1;play_from_start=1;schedule(4000);
    CHECK(!play_pending&&!want_start&&!starting&&strstr(status,"too slow to play (64 kHz)"));
    /* the climb reached 256 kHz: the title starts */
    settle();pj_ln.khz=256;link_ceiling_khz=256;play_pending=1;play_from_start=1;schedule(5000);
    CHECK(!play_pending&&starting&&io_busy&&io_command==PJ_RPC_START);
    reply(PJ_RPC_START,"{\"session\":60}");CHECK(playing);
    press(OF_BTN_B);CHECK(!playing);stop_answered();
    /* The stream capability is read from every later HELLO answer: a Chromatic whose fast
     * connection was not up at "found" ("none") lights the titles when it is, and dims them if it
     * fails (before: unavailable for the whole session). */
    settle();pj_ln.khz=512;link_good_khz=256;link_ceiling_khz=256;screen=PJ_SCREEN_HOME;home=1;set_status("");
    const char *none="{\"version\":1,\"compressed_stream\":\"none\"}";
    const char *jpeg="{\"version\":1,\"compressed_stream\":\"PJAV-JPEG-IMA\"}";
    CHECK(link_streams&&!want_reload);
    hello_again((const uint8_t *)jpeg,strlen(jpeg));CHECK(link_streams&&!want_reload);
    hello_again((const uint8_t *)none,strlen(none));CHECK(!link_streams&&want_reload);
    screen=PJ_SCREEN_DETAILS;schedule(6000);CHECK(want_reload&&!(io_busy&&io_command==PJ_RPC_HOME));
    settle();screen=PJ_SCREEN_HOME;schedule(6000);CHECK(!want_reload&&io_busy&&io_command==PJ_RPC_HOME);
    settle();CHECK(handle_reply(PJ_RPC_HOME,home_data,home_size));
    CHECK(!library.items[0].folder&&library.items[0].unavailable);
    hello_again((const uint8_t *)jpeg,strlen(jpeg));CHECK(link_streams&&want_reload);
    settle();screen=PJ_SCREEN_HOME;schedule(7000);CHECK(!want_reload&&io_busy&&io_command==PJ_RPC_HOME);
    settle();CHECK(handle_reply(PJ_RPC_HOME,home_data,home_size));
    CHECK(!library.items[0].unavailable);
    /* A HELLO answer too long for this app says so (before: closed and retried without words). */
    settle();size_t old_size=rpc.response_size;rpc.response_size=PJ_RPC_HEADER+1025u;temp_sign[0]=0;temp_until_us=0;
    tether_found(8000);
    CHECK(!strncmp(temp_sign,"Update the Pocket app",21)&&temp_until_us&&io==&link_source&&online);
    rpc.response_size=old_size;temp_until_us=0;temp_sign[0]=0;
    CHECK(pj_link_transport_open(&link_source,&rpc,NULL));
    /* as main left it */
    settle();pj_ln.khz=512;pj_ln.slowest_khz=slowest;link_good_khz=0;link_ceiling_khz=PJ_LINK_SAFE_KHZ;climb_retry_us=0;
    screen=PJ_SCREEN_HOME;home=1;selected=1;want_home=want_browse=want_start=0;
}
/* F2 (a): the playback queues themselves (live_playback.h), with a sound device that takes nothing. */
#include "live_playback.h"
static pj_live_player pb;
static unsigned pb_free=8192;
static void pb_reset(void *c){(void)c;}
static unsigned pb_audio_free(void *c){(void)c;return pb_free;}
static void pb_abort(void *c){(void)c;}
static size_t av_packet(uint8_t *out,unsigned kind,unsigned flags,uint32_t sequence,uint64_t pts,const uint8_t *payload,uint32_t size) {
    memset(out,0,PJ_AV_HEADER);memcpy(out,"PJAV",4);out[4]=1;out[5]=(uint8_t)kind;out[6]=(uint8_t)flags;
    pj_put32(out+8,7);pj_put32(out+12,sequence);pj_put32(out+16,(uint32_t)pts);pj_put32(out+20,(uint32_t)(pts>>32));
    pj_put32(out+24,size);if(size)memcpy(out+PJ_AV_HEADER,payload,size);
    pj_put32(out+28,pj_av_crc_part(pj_av_crc_part(0xffffffffu,out,28),out+PJ_AV_HEADER,size)^0xffffffffu);
    return PJ_AV_HEADER+size;
}
static void playback_checks(void) {
    pj_live_ops ops={0};ops.audio_reset=pb_reset;ops.audio_free=pb_audio_free;ops.video_abort=pb_abort;
    pj_live_init(&pb,ops);
    uint8_t packet[PJ_AV_HEADER+160],config[20]={0},picture[100],sound[128]={0};
    config[0]=128;config[2]=96;config[4]=30;config[6]=1;config[8]=0x80;config[9]=0x3e; /* 16,000 Hz */
    config[12]=1;config[14]=1;config[16]=128;memset(picture,0x55,sizeof(picture));
    CHECK(pj_live_feed(&pb,packet,av_packet(packet,PJ_AV_CONFIG,0,0,0,config,20))==1&&pb.configured);
    uint32_t seq=1;
    /* A stream that brings pictures before its sound: the queue fills (32) and the pictures after
     * that are taken and given up, so the reads that bring the sound go on. Before, the 33rd was
     * held back for ever: no more reads, no sound, playback never started. */
    for(unsigned i=0;i<40;i++)
        CHECK(pj_live_feed(&pb,packet,av_packet(packet,PJ_AV_VIDEO,PJ_AV_KEYFRAME,seq++,(uint64_t)i*3000u,picture,sizeof(picture)))==1);
    CHECK(pb.vcount==PJ_LIVE_VIDEO_QUEUE&&pb.dropped==8&&!pb.started&&!pb.failed);
    CHECK(pb.receiver.next_sequence==seq&&!pb.receiver.lost_packets);
    CHECK(pb.video[0].pts==0&&pb.video[31].pts==31u*3000u); /* the queued ones keep their times */
    CHECK(pj_live_feed(&pb,packet,av_packet(packet,PJ_AV_AUDIO,0,seq++,0,sound,128))==1&&pb.acount==249u*3u);
    /* Playing with sound in hand (over 50 ms beyond what is held back, queued here or in the
     * sound device): a full queue holds the packet back as before, and nothing advances. */
    size_t n=av_packet(packet,PJ_AV_VIDEO,PJ_AV_KEYFRAME,seq,41u*3000u,picture,sizeof(picture));
    uint64_t dropped=pb.dropped;
    pb.started=1;pb.acount=pb.hold_frames+2401u;
    CHECK(pj_live_feed(&pb,packet,n)==0&&pb.dropped==dropped&&pb.receiver.next_sequence==seq&&pb.vcount==PJ_LIVE_VIDEO_QUEUE);
    pb.acount=0;pb_free=8192u-(pb.hold_frames+2401u);
    CHECK(pj_live_feed(&pb,packet,n)==0&&pb.dropped==dropped);
    /* Playing with the sound nearly gone: the picture is given up so the sound can come. */
    pb_free=8192u-(pb.hold_frames+2400u);
    CHECK(pj_live_feed(&pb,packet,n)==1&&pb.dropped==dropped+1&&pb.receiver.next_sequence==seq+1&&!pb.failed);
}
/* 2026-11-10 Core Settings: the word (pocket/core_settings.h) and what the player does with Buffer and Volume. */
static void settings_checks(void) {
    pj_core_settings s=pj_settings_defaults();uint8_t code[PJ_SET_FIELDS];
    CHECK(pj_settings_pack(&s)==0x00001311u&&pj_settings_buffer_mode(&s)==PJ_LIVE_BUFFER_NORMAL&&pj_settings_buffer_seconds(&s)==2u&&!pj_settings_fields(0,code));
    CHECK(!pj_settings_fields(0x12345678u,code)); /* not a settings word: nothing taken */
    /* the 2026-11-10 tagged form (both halves set) and its other byte order (0x4a50: codes out of range) are not taken */
    CHECK(!pj_settings_fields(0x504a1311u,code)&&!pj_settings_fields(0x504a0000u,code)&&!pj_settings_merge(&s,0x504a0004u));
    /* one entry's write: the Pocket read 0 from the core, so it carries its own field alone; only that field changes */
    CHECK(pj_settings_merge(&s,0x00000004u)==1u&&s.code[PJ_SET_FPS]==PJ_FPS_60&&pj_settings_fps(&s)==60u);
    CHECK(pj_settings_merge(&s,0x00002000u)==8u&&pj_settings_gain_shift(&s)==1u&&s.code[PJ_SET_FPS]==PJ_FPS_60);
    CHECK(pj_settings_merge(&s,pj_settings_swap(0x00000200u))==4u&&pj_settings_buffer_mode(&s)==PJ_LIVE_BUFFER_LONG); /* the other byte order */
    CHECK(pj_settings_merge(&s,pj_settings_swap(0x00000030u))==2u&&s.code[PJ_SET_LINK]==PJ_LINKSET_FAST);
    CHECK(!pj_settings_merge(&s,0x00002234u)); /* all four as they are (a Pocket that kept the others): nothing changed */
    CHECK(pj_settings_merge(&s,0x0000000fu)==0u&&s.code[PJ_SET_FPS]==PJ_FPS_60); /* a code out of range is not taken */
    CHECK(pj_settings_merge(&s,0x00004001u)==0u&&s.code[PJ_SET_FPS]==PJ_FPS_60); /* nor is anything in a word that has one */
    s.code[PJ_SET_FPS]=PJ_FPS_15;CHECK(pj_settings_fps(&s)==24u);s.code[PJ_SET_FPS]=PJ_FPS_FOLLOW;CHECK(!pj_settings_fps(&s));
    char words[80];CHECK(strstr(pj_settings_text(&s,words,sizeof(words)),"link Fast 512")&&strstr(words,"+6 dB")&&strstr(words,"Long 4 s"));
    s.code[PJ_SET_BUFFER]=PJ_BUFSET_SHORT;CHECK(pj_settings_buffer_mode(&s)==PJ_LIVE_BUFFER_SHORT&&!pj_settings_buffer_seconds(&s));
    s.code[PJ_SET_VOLUME]=PJ_VOLSET_12DB;CHECK(pj_settings_gain_shift(&s)==2u);
    pj_link_mode a=pj_link_mode_for(PJ_LINKSET_AUTO),f=pj_link_mode_for(PJ_LINKSET_FAST),z=pj_link_mode_for(PJ_LINKSET_SAFE);
    CHECK(a.acks==1&&a.max_khz==384&&a.fill_slow_us==8000&&f.acks==0&&f.max_khz==512&&f.fill_slow_us==10000&&
          z.acks==1&&z.max_khz==256&&z.fill_slow_us==10000);
    /* The app takes the menu: frame rate, buffer and volume at once; the link cap at the next HELLO and in schedule. */
    pj_settings_host_word=0x00002212u;settings_check_us=0;quality=1; /* 24 fps, Auto, Long, +6 dB */
    settings_poll(1000000u);
    CHECK(quality==0&&set_buffer==1&&set_gain==1&&pj_link_cfg.acks==1&&pj_link_cfg.max_khz==384&&link_setting_pending);
    CHECK(store.settings==0x00002212u&&pj_store_busy(&store)); /* kept beside the watch state */
    unsigned calls=set_calls;pj_settings_host_word=0x00000003u;settings_poll(1000001u);CHECK(quality==0); /* four times a second */
    settings_poll(1300000u);CHECK(quality==1&&set_calls==calls&&store.settings==0x00002213u);
    pj_settings_host_word=0;settings_poll(1600000u);CHECK(store.settings==0x00002213u&&quality==1); /* 0: nothing changes */
    /* Back to the build's link figures for the checks after this one. */
    pj_link_cfg=pj_link_mode_for(PJ_LINKSET_FAST);link_setting_pending=0;quality=1;memset(&store,0,sizeof(store));
    /* Volume: +6 and +12 dB double and quadruple, saturated, never wrapped. */
    pj_live_options o=pj_live_buffer_options(PJ_LIVE_BUFFER_SHORT,2);pj_live_set_options(&pb,o);
    CHECK(pj_live_gain(&pb,1000)==4000&&pj_live_gain(&pb,9000)==32767&&pj_live_gain(&pb,-9000)==-32768&&pb.clipped==2);
    o.gain_shift=1;pj_live_set_options(&pb,o);CHECK(pj_live_gain(&pb,-16384)==-32768&&pj_live_gain(&pb,16383)==32766);
    /* Long buffer: a store of compressed pictures, hundreds of places; a CONFIG keeps the options. */
    pj_live_ops ops={0};ops.audio_reset=pb_reset;ops.audio_free=pb_audio_free;ops.video_abort=pb_abort;
    pb_free=8192;pj_live_init(&pb,ops);
    CHECK(pb.opts.video_cap==PJ_LIVE_VIDEO_QUEUE&&pb.opts.audio_cap==PJ_LIVE_AUDIO_FRAMES&&pb.opts.start_frames==PJ_LIVE_START_FRAMES);
    pj_live_set_options(&pb,pj_live_buffer_options(PJ_LIVE_BUFFER_LONG,0));
    uint8_t packet[PJ_AV_HEADER+PJ_AV_LIMIT],config[20]={0};static uint8_t picture[PJ_AV_LIMIT];
    config[0]=128;config[2]=96;config[4]=30;config[6]=1;config[8]=0x80;config[9]=0x3e;config[12]=1;config[14]=1;config[16]=128;
    CHECK(pj_live_feed(&pb,packet,av_packet(packet,PJ_AV_CONFIG,0,0,0,config,20))==1&&pb.opts.video_cap==PJ_LIVE_VIDEO_SLOTS);
    CHECK(pj_live_target(&pb,pb.opts.start_frames)==pb.opts.start_frames); /* 4 s fits 508 places at 30 fps */
    uint32_t seq=1;memset(picture,0x5a,sizeof(picture));
    for(unsigned i=0;i<300;i++){picture[0]=(uint8_t)i;CHECK(pj_live_feed(&pb,packet,av_packet(packet,PJ_AV_VIDEO,PJ_AV_KEYFRAME,seq++,(uint64_t)i*3000u,picture,2000))==1);}
    CHECK(pb.vcount==300&&!pb.dropped&&!pb.store_full);
    for(unsigned i=0;i<300;i++)CHECK(pb.vbytes[pb.video[i].at]==(uint8_t)i&&pb.video[i].size==2000&&pb.video[i].pts==(uint64_t)i*3000u);
    /* The largest pictures: the bytes run out before the places, and the buffer then starts with what it has. */
    pj_live_init(&pb,ops);pj_live_set_options(&pb,pj_live_buffer_options(PJ_LIVE_BUFFER_LONG,0));seq=1;
    CHECK(pj_live_feed(&pb,packet,av_packet(packet,PJ_AV_CONFIG,0,0,0,config,20))==1);
    unsigned kept=0;
    for(unsigned i=0;i<80;i++)if(pj_live_feed(&pb,packet,av_packet(packet,PJ_AV_VIDEO,PJ_AV_KEYFRAME,seq++,(uint64_t)i*3000u,picture,PJ_AV_LIMIT))==1&&pb.vcount>kept)kept=pb.vcount;
    CHECK(kept==PJ_LIVE_VIDEO_BYTES/PJ_AV_LIMIT&&pb.vcount==kept&&pb.dropped==80u-kept&&pb.store_full);
    /* The oldest picture's first byte is never written over (the decoder reads it until it is done): with one
     * popped, a largest picture would end exactly there and is given up; with two it goes at the start again. */
    pj_live_vpop(&pb);uint64_t lost=pb.dropped;
    CHECK(pj_live_feed(&pb,packet,av_packet(packet,PJ_AV_VIDEO,PJ_AV_KEYFRAME,seq++,81u*3000u,picture,PJ_AV_LIMIT))==1&&pb.dropped==lost+1);
    pj_live_vpop(&pb);
    CHECK(pj_live_feed(&pb,packet,av_packet(packet,PJ_AV_VIDEO,PJ_AV_KEYFRAME,seq++,82u*3000u,picture,PJ_AV_LIMIT))==1&&pb.dropped==lost+1);
    CHECK(pb.vcount==kept-1u&&pb.video[(pb.vhead+pb.vcount-1)%PJ_LIVE_VIDEO_SLOTS].at==0u&&pb.vtail==PJ_AV_LIMIT);
    /* Normal: 2 s to start, the same store and caps as Long. */
    pj_live_options n=pj_live_buffer_options(PJ_LIVE_BUFFER_NORMAL,0);
    CHECK(n.start_frames==96000u&&n.rebuffer_frames==96000u&&n.video_cap==PJ_LIVE_VIDEO_SLOTS&&n.audio_cap==8u*48000u);
    /* Short (the buffer of before): never limited by bytes, 32 places, as playback_checks found. */
    pj_live_init(&pb,ops);seq=1;
    CHECK(pj_live_feed(&pb,packet,av_packet(packet,PJ_AV_CONFIG,0,0,0,config,20))==1);
    for(unsigned i=0;i<40;i++)pj_live_feed(&pb,packet,av_packet(packet,PJ_AV_VIDEO,PJ_AV_KEYFRAME,seq++,(uint64_t)i*3000u,picture,PJ_AV_LIMIT));
    CHECK(pb.vcount==PJ_LIVE_VIDEO_QUEUE&&pb.dropped==8&&!pb.store_full);
}
/* 2026-11-10: the card's own library from the Chromatic's Home. With a card library the Home ends My Media with the
 * app's "On this Pocket" tile (its own row at the end when there is no My Media), never on the card's own Home or
 * without a card, and no picture is asked for it. Opening it and playing from the card: the Jellymatic tether simulation. */
static void card_tile_checks(void) {
    static const char with[]="{\"rows\":["
    "{\"title\":\"My Media\",\"shape\":0,\"items\":[{\"handle\":31,\"title\":\"Movies\",\"duration_ticks\":0,\"resume_ticks\":0,\"folder\":true,\"played\":false}]},"
    "{\"title\":\"Next Up\",\"shape\":0,\"items\":[{\"handle\":41,\"title\":\"Sample Episode\",\"duration_ticks\":10,\"resume_ticks\":0,\"folder\":false,\"played\":false}]}],"
    "\"truncated\":false}";
    static const char without[]="{\"rows\":["
    "{\"title\":\"Next Up\",\"shape\":1,\"items\":[{\"handle\":41,\"title\":\"Sample Episode\",\"duration_ticks\":10,\"resume_ticks\":0,\"folder\":false,\"played\":false}]}],"
    "\"truncated\":false}";
    settle();have_stored=1;stored.title_count=3;selected=0;
    CHECK(handle_reply(PJ_RPC_HOME,(const uint8_t *)with,strlen(with)));
    CHECK(library.count==3&&library.row_count==2&&library.rows[0].count==2&&library.rows[1].first==2&&library.rows[1].count==1);
    const pj_library_item *t=&library.items[1];
    CHECK(t->card_tile&&t->folder&&t->handle==PJ_CARD_TILE_HANDLE&&!strcmp(t->title,"On this Pocket")&&!strcmp(t->subtitle,"3 videos on the SD card"));
    CHECK(!strcmp(library.items[2].title,"Sample Episode")&&!library.items[0].card_tile&&!library.items[2].card_tile);
    /* Right from Movies reaches it; no picture or backdrop is asked for it */
    screen=PJ_SCREEN_HOME;home=1;selected=0;press(OF_BTN_RIGHT);CHECK(selected==1);
    reset_art();store_synthetic(31,PJ_ART_THUMB);focus_us=0;CHECK(!next_art(10000000u)&&!art.handle);
    /* no My Media: its own row at the end */
    stored.title_count=1;CHECK(handle_reply(PJ_RPC_HOME,(const uint8_t *)without,strlen(without)));
    CHECK(library.count==2&&library.row_count==2&&!strcmp(library.rows[1].title,"On this Pocket")&&library.rows[1].first==1&&
          library.items[1].card_tile&&!strcmp(library.items[1].subtitle,"1 video on the SD card"));
    /* the card's own Home, and no card: no tile */
    pj_transport *was=io;io=&stored_source;
    CHECK(handle_reply(PJ_RPC_HOME,(const uint8_t *)with,strlen(with))&&library.count==2);
    io=was;have_stored=0;
    CHECK(handle_reply(PJ_RPC_HOME,(const uint8_t *)with,strlen(with))&&library.count==2);
    for(unsigned i=0;i<library.count;i++)CHECK(!library.items[i].card_tile);
    settle();selected=0;set_status("");
}
int main(int argc,char **argv) {
    CHECK(argc==3);rpc_checks();
    /* B on the start screen never leaves the app (that dropped users into the OS menu). */
    press(OF_BTN_B);CHECK(screen==PJ_SCREEN_SOURCE&&!online&&strstr(status,"Analogue"));
    /* Y opens the About page from the start screen; B (or Y) returns to it. */
    press(OF_BTN_Y);CHECK(screen==PJ_SCREEN_CREDITS&&!online&&!io);
    press(OF_BTN_A);CHECK(screen==PJ_SCREEN_CREDITS&&!online&&!io);
    press(OF_BTN_B);CHECK(screen==PJ_SCREEN_SOURCE&&!online);
    press(OF_BTN_Y);press(OF_BTN_Y);CHECK(screen==PJ_SCREEN_SOURCE);
    CHECK(pj_link_transport_open(&link_source,&rpc,NULL));io=&link_source;
    CHECK(!handle_reply(PJ_RPC_HELLO,(const uint8_t *)"{\"version\":1}",13));
    reply(PJ_RPC_HELLO,"{\"version\":1,\"compressed_stream\":\"PJAV-JPEG-IMA\",\"link_khz\":2000}");
    /* The rate the peer names is only a wish, capped at 512 kHz; nothing changes on its word. */
    CHECK(online&&want_home&&link_want_khz==PJ_LINK_TOP_KHZ&&link_khz==256&&pj_ln.khz==256);
    /* Before Home is asked, the next rate up is tried: a still clock first, no request yet. */
    schedule(200);CHECK(gate==1&&gate_khz==512&&gate_owner==OWNER_PROBE&&!io_busy&&pj_ln.hold&&want_home&&link_khz==256);
    /* The rest of this check is the controller at the found rate (the climb: the Jellymatic tether simulation). */
    gate=0;pj_ln.hold=0;link_ceiling_khz=PJ_LINK_SAFE_KHZ;
    want_home=0;FILE *f=fopen(argv[1],"rb");CHECK(f);
    uint8_t data[32784];size_t n=fread(data,1,sizeof(data),f);CHECK(feof(f));fclose(f);
    CHECK(pj_rpc_validate(data,n));CHECK(handle_reply(PJ_RPC_HOME,data+16,n-16));
    CHECK(screen==PJ_SCREEN_HOME&&library.rows[0].shape==PJ_ART_POSTER);
    /* A opens details first; details request is cheap and never blocks playback. */
    press(OF_BTN_A);CHECK(screen==PJ_SCREEN_DETAILS&&want_details);
    request_check(PJ_RPC_DETAILS,0,NULL);reply(PJ_RPC_DETAILS,"{\"overview\":\"Synthetic\",\"genres\":[\"Drama\"]}");
    CHECK(details.valid&&!strcmp(details.genres,"Drama"));
    press(OF_BTN_A);request_check(PJ_RPC_START,5,"pocket-link-256-30");CHECK(starting);
    press(OF_BTN_B);CHECK(cancel_start);
    reply(PJ_RPC_START,"{\"session\":37}");CHECK(!playing&&want_stop&&opens==0);
    request_check(PJ_RPC_STOP,0,NULL);reply(PJ_RPC_STOP,"{\"stopped\":true}");CHECK(!session);
    press(OF_BTN_A);request_check(PJ_RPC_START,5,"pocket-link-256-30");
    reply(PJ_RPC_START,"{\"session\":38}");CHECK(playing&&opens==1&&base_seconds==5);
    model_stats.clock_pts=12*90000;
    press(OF_BTN_START);CHECK(paused&&!playing&&want_stop&&position_seconds==17&&screen==PJ_SCREEN_DETAILS);
    request_check(PJ_RPC_STOP,0,NULL);reply(PJ_RPC_STOP,"{\"stopped\":true}");
    press(OF_BTN_START);request_check(PJ_RPC_START,17,"pocket-link-256-30");
    reply(PJ_RPC_START,"{\"session\":39}");CHECK(playing&&!paused&&base_seconds==17);
    model_stats.clock_pts=2*90000;
    press(OF_BTN_SELECT);CHECK(quality==2&&want_stop&&restart&&position_seconds==19);
    request_check(PJ_RPC_STOP,0,NULL);reply(PJ_RPC_STOP,"{\"stopped\":true}");
    /* 60 FPS chosen on a 256 kHz link: the choice is kept and the stream is asked at 30 FPS. */
    request_check(PJ_RPC_START,19,"pocket-link-256-30");CHECK(stream_quality==1&&quality==2);
    reply(PJ_RPC_START,"{\"session\":40}");model_stats.clock_pts=4*90000;
    pj_ln.khz=512; /* as after a proven climb (Jellymatic tether simulation): now the choice itself is asked */
    press(OF_BTN_RIGHT);CHECK(position_seconds==9); /* ten-second fixture clamps to last second */
    request_check(PJ_RPC_STOP,0,NULL);reply(PJ_RPC_STOP,"{\"stopped\":true}");
    request_check(PJ_RPC_START,9,"pocket-link-512-60");
    /* A stored source snaps to its seek point and says so; the clock must follow. */
    reply(PJ_RPC_START,"{\"session\":41,\"start_seconds\":5}");CHECK(base_seconds==5&&position_seconds==5);
    uint8_t wrong_read[16]={0};pj_put32(wrong_read,42);wrong_read[12]=2;
    CHECK(!handle_reply(PJ_RPC_READ,wrong_read,16));
    press(OF_BTN_B);request_check(PJ_RPC_STOP,0,NULL);reply(PJ_RPC_STOP,"{\"stopped\":true}");
    CHECK(!playing&&!paused&&!session&&closes>0&&screen==PJ_SCREEN_DETAILS);
    press(OF_BTN_B);CHECK(screen==PJ_SCREEN_HOME);
    /* Row navigation: left/right stay in a row, down moves rows and keeps the column. */
    selected=0;unsigned rows=library.row_count;CHECK(rows>=2);
    io_busy=1;io_command=PJ_RPC_ART; /* navigation stays live while artwork loads */
    press(OF_BTN_RIGHT);CHECK(selected==1);
    press(OF_BTN_DOWN);CHECK(selected==library.rows[1].first);
    press(OF_BTN_UP);CHECK(selected==0);
    press(OF_BTN_RIGHT);press(OF_BTN_A);CHECK(want_browse&&parent==library.items[1].handle&&depth==1);
    io_busy=0;rpc.waiting=rpc.link.tx_active=0;request_check(PJ_RPC_BROWSE,0,NULL);
    io_busy=0;rpc.waiting=rpc.link.tx_active=0;want_browse=0;
    home=0;library.next=12;library.total=40;press(OF_BTN_R1);CHECK(page==12&&want_browse);
    /* B in a folder opened from Home (like "On this Pocket") returns to Home on the same
     * tile, not to a browse of "no folder" that the card refuses. */
    want_browse=0;press(OF_BTN_B);CHECK(want_home&&!want_browse&&depth==0&&selected==1);
    want_home=0;
    want_browse=0;home=1;
    /* A refused title leaves the source connected. */
    starting=1;handle_refused(PJ_RPC_START);CHECK(!starting&&online);
    link_fix_checks(data+16,n-16);playback_checks();settings_checks();card_tile_checks();
    render_previews(argv[2]);
    puts("PASS: actual native controller details/start/cancel/pause/resume/seek/quality/rows/browse/page/read guards, About page and all native screens; modeled link/SDK IO only.");
    return 0;
}
