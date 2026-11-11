/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * SDK DAC/framebuffer + hardware JPEG binding used by live_player.c.
 */
#include <string.h>
#include <stdio.h>
#include "live_session.h"
#include "live_pocket_backend.h"
#include "selftest_jpeg.h"
#include "of_video.h"
#include "of_caps.h"
#include "of_timer.h"
#define OSD_ROWS 30u
static pj_live_player player;
static pj_live_options options={PJ_LIVE_START_FRAMES,PJ_LIVE_REBUFFER_FRAMES,PJ_LIVE_AUDIO_FRAMES,PJ_LIVE_VIDEO_QUEUE,0};
static pj_live_pocket backend;
static int opened,selftest_ok;
static unsigned last_w,last_h,draw_w,draw_h,left,top,clear_frames,osd_clear;
static uint16_t sample_x[320],sample_y[240];
static uint16_t osd_strip[320*OSD_ROWS];
static int film_mode=1;
static unsigned refresh_lines,frames_seen;
static uint64_t first_pts;
static void (*osd_draw)(uint16_t *);
static int (*osd_active)(uint32_t);
void pj_live_session_set_osd(void (*draw)(uint16_t *),int (*active)(uint32_t)) {
    osd_draw=draw;osd_active=active;
}
/* Scanout is 24.576 MHz / (780 * lines), and the scaler locks 47-61 Hz. Showing a
 * film frame exactly twice at 48 Hz avoids the uneven 3:2 cadence 60 Hz forces.
 * 23.976 and 29.97 sources differ from 24 and 30 by 0.1%, and only their
 * timestamps can tell: `slow` means they ran that much longer than nominal. */
static unsigned film_lines(unsigned fps,int slow) {
    switch(fps) {
    case 24:return slow?657u:656u;  /* 47.96 / 48.03 Hz */
    case 25:return 630u;            /* 50.01 Hz */
    case 30:return slow?526u:525u;  /* 59.90 / 60.02 Hz */
    default:return 0;               /* no whole multiple: leave it to the firmware */
    }
}
static void apply_refresh(unsigned lines) {
    refresh_lines=lines;of_video_set_refresh_vtotal(lines);
}
static void track_cadence(uint64_t pts) {
    unsigned fps=player.receiver.fps;
    if(!frames_seen)first_pts=pts;
    frames_seen++;
    if(!film_mode)return;
    if(frames_seen==1)apply_refresh(film_lines(fps,0));
    else if(frames_seen==fps+1) {
        uint64_t nominal=90000u; /* fps frames should span one second */
        int slow=pts-first_pts>nominal+nominal/2000u;
        unsigned lines=film_lines(fps,slow);
        if(lines!=refresh_lines)apply_refresh(lines);
    }
}
static void show_frame(const uint16_t *pixels,unsigned width,unsigned height,uint64_t pts) {
    track_cadence(pts);
    if(width!=last_w||height!=last_h) {
        draw_w=320;draw_h=height*320/width;
        if(draw_h>240){draw_h=240;draw_w=width*240/height;}
        for(unsigned x=0;x<draw_w;x++)sample_x[x]=(uint16_t)(x*width/draw_w);
        for(unsigned y=0;y<draw_h;y++)sample_y[y]=(uint16_t)(y*height/draw_h);
        left=(320-draw_w)/2;top=(240-draw_h)/2;last_w=width;last_h=height;
        clear_frames=3; /* the runtime rotates three buffers */
    }
    of_video_mode_t mode;of_video_get_mode(&mode);
    uint8_t *surface=of_video_surface();
    /* Bars never change, so only repaint them until every buffer has them. */
    if(clear_frames) {
        for(unsigned y=0;y<240;y++)memset(surface+y*mode.stride,0,640);
        clear_frames--;
    }
    for(unsigned y=0;y<draw_h;y++) {
        uint16_t *row=(uint16_t *)(surface+(top+y)*mode.stride)+left;
        /* Upscaling repeats rows; copying one beats resampling it again. */
        if(y&&sample_y[y]==sample_y[y-1]) {
            memcpy(row,(uint16_t *)(surface+(top+y-1)*mode.stride)+left,draw_w*2);
            continue;
        }
        const uint16_t *source=pixels+sample_y[y]*width;
        for(unsigned x=0;x<draw_w;x++)row[x]=source[sample_x[x]];
    }
    if(osd_draw&&osd_active&&top+draw_h+OSD_ROWS<=240) {
        if(osd_active(of_time_us())) {
            osd_draw(osd_strip);
            for(unsigned y=0;y<OSD_ROWS;y++)
                memcpy(surface+(240-OSD_ROWS+y)*mode.stride,osd_strip+y*320,640);
            osd_clear=3;
        } else if(osd_clear) {
            for(unsigned y=240-OSD_ROWS;y<240;y++)memset(surface+y*mode.stride,0,640);
            osd_clear--;
        }
    }
    of_video_flip(); /* queues the buffer; does not wait for vsync */
}
int pj_live_session_open(void) {
    if(opened)pj_live_session_close();
    if(!of_get_caps())return 0;
    pj_live_ops ops;
    if(!pj_live_pocket_ops(&backend,&ops,show_frame))return 0;
    of_video_init();
    of_video_set_color_mode(OF_VIDEO_MODE_RGB565);
    of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
    of_video_mode_t mode;of_video_get_mode(&mode);
    if(mode.color_mode!=OF_VIDEO_MODE_RGB565||mode.stride<640)return 0;
    pj_live_init(&player,ops);pj_live_set_options(&player,options);
    last_w=last_h=0;osd_clear=0;frames_seen=0;opened=1;return 1;
}
void pj_live_session_set_options(unsigned buffer,unsigned gain_shift) {
    options=pj_live_buffer_options(buffer,gain_shift);
    if(opened)pj_live_set_options(&player,options);
}
void pj_live_session_close(void) {
    if(opened) {
        pj_live_stop(&player);
        if(refresh_lines)apply_refresh(0); /* menus go back to automatic */
    }
    opened=0;
}
int pj_live_session_toggle_film(void) {
    film_mode=!film_mode;
    if(!film_mode)apply_refresh(0);
    else if(opened&&player.receiver.fps)apply_refresh(film_lines(player.receiver.fps,0));
    return film_mode;
}
int pj_live_session_push(const uint8_t *packet,size_t size) {
    return opened?pj_live_feed(&player,packet,size):-1;
}
int pj_live_session_tick(uint32_t now_us) {
    return opened?pj_live_service(&player,now_us):-1;
}
void pj_live_session_status(pj_live_session_stats *stats) {
    if(!stats)return;
    memset(stats,0,sizeof(*stats));
    if(!opened)return;
    stats->shown=player.shown;stats->dropped=player.dropped;stats->audio_starves=player.audio_starves;
    stats->clock_pts=player.clock*90000/48000;stats->fps=player.receiver.fps;
    stats->width=player.receiver.width;stats->height=player.receiver.height;
    stats->rebuffers=player.rebuffers;stats->rebuffering=player.rebuffering;
    stats->refresh_lines=refresh_lines;stats->buffering=!player.started;stats->complete=player.complete;stats->failed=player.failed;
    stats->why=player.why?player.why:"";
    stats->buffered_tenths=pj_live_buffered_tenths(&player);stats->clipped=player.clipped;
    stats->jpeg_fail=backend.jpeg.fail;stats->jpeg_pixels=backend.jpeg.fail_pixels;
    stats->jpeg_faults=backend.jpeg.fail_faults;stats->jpeg_ms=backend.jpeg.fail_us/1000u;
}
int pj_live_session_selftest_passed(void) {return selftest_ok;}
void pj_live_session_selftest(pj_live_selftest_result *result) {
    static uint16_t pixels[PJ_SELFTEST_W*PJ_SELFTEST_H];
    selftest_ok=0;
    pj_pocket_jpeg *jpeg=&backend.jpeg;
    if(!pj_jpeg_open(jpeg,(volatile uint32_t *)(uintptr_t)PJ_JPEG_BASE)) {
        snprintf(result->message,sizeof(result->message),"not found (rev %u needed)",PJ_JPEG_REVISION);
        result->passed=0;return;
    }
    uint32_t start=of_time_us();
    if(!pj_jpeg_begin(jpeg,pj_selftest_jpeg,sizeof(pj_selftest_jpeg),pixels,PJ_SELFTEST_W,PJ_SELFTEST_H,start)) {
        snprintf(result->message,sizeof(result->message),"refused test frame");result->passed=0;return;
    }
    int state;
    do state=pj_jpeg_service(jpeg,8192,of_time_us());while(!state);
    uint32_t elapsed=of_time_us()-start;
    if(state<0){snprintf(result->message,sizeof(result->message),"test frame FAULT");result->passed=0;return;}
    uint32_t crc=pj_av_crc_part(0xffffffffu,(const uint8_t *)pixels,sizeof(pixels))^0xffffffffu;
    selftest_ok=result->passed=crc==PJ_SELFTEST_CRC;
    snprintf(result->message,sizeof(result->message),selftest_ok?"ok, test frame %lu us":"PIXELS WRONG (%lu us)",(unsigned long)elapsed);
}
