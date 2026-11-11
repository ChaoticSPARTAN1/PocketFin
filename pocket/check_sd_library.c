/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Runs the actual on-card source against real library/pack files, through the
 * actual stream reader and playback engine, with libjpeg standing in for the FPGA
 * decoder and a modeled DAC. Synthetic media only. No hardware.
 */
#define OF_PC 1
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <jpeglib.h>
#include "sd_library.h"
#include "live_library.h"
#include "live_playback.h"
#include "stream_reader.h"
#include "live_ui.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"sd library check failed at %d\n",__LINE__);exit(1);}} while(0)
static const char *library_path,*pack_path;
int pj_sd_pc_read(unsigned slot,uint32_t offset,void *dest,uint32_t size) {
    FILE *f=fopen(slot==PJ_SD_LIBRARY_SLOT?library_path:slot==20?pack_path:"",  "rb");
    if(!f)return -1;
    int ok=!fseek(f,(long)offset,SEEK_SET)&&fread(dest,1,size,f)==size;
    fclose(f);return ok?0:-1;
}
static uint32_t fake_clock;
static uint32_t clock_us(void){return fake_clock;}
static pj_sd_library sd;
static pj_transport io;
static pj_live_library library;
static pj_details details;
static pj_art_download art;
static pj_stream_reader stream;
static pj_live_player player;
typedef struct {unsigned queued,active,presented;uint64_t written;} backend;
static backend model;
static void audio_reset(void *c){((backend *)c)->queued=0;}
static unsigned audio_free(void *c){return 131072-((backend *)c)->queued;}
static unsigned audio_write(void *c,const int16_t *s,unsigned n) {
    backend *b=c;(void)s;if(n>audio_free(c))n=audio_free(c);
    b->queued+=n;b->written+=n;return n;
}
typedef struct {struct jpeg_error_mgr base;jmp_buf jump;} jpeg_error;
static void jpeg_fail(j_common_ptr c){longjmp(((jpeg_error *)c->err)->jump,1);}
static int video_begin(void *c,const uint8_t *data,size_t size,uint16_t *frame,unsigned w,unsigned h,uint32_t now) {
    (void)frame;(void)now;
    struct jpeg_decompress_struct d;jpeg_error e;memset(&d,0,sizeof(d));
    d.err=jpeg_std_error(&e.base);e.base.error_exit=jpeg_fail;
    if(setjmp(e.jump)){jpeg_destroy_decompress(&d);return 0;}
    jpeg_create_decompress(&d);jpeg_mem_src(&d,data,(unsigned long)size);
    /* Tables are stripped on the wire, exactly as the FPGA expects: only check SOF. */
    (void)w;(void)h;jpeg_destroy_decompress(&d);
    ((backend *)c)->active=1;return 1;
}
static int video_poll(void *c,unsigned budget,uint32_t now){(void)budget;(void)now;((backend *)c)->active=0;return 1;}
static void video_abort(void *c){((backend *)c)->active=0;}
static void present(void *c,const uint16_t *f,unsigned w,unsigned h,uint64_t pts) {
    (void)f;CHECK(w==256&&h==144);(void)pts;((backend *)c)->presented++;
}
static const uint8_t *ask(unsigned command,const char *json,size_t *size) {
    CHECK(io.idle(io.context));
    CHECK(io.begin(io.context,command,json,fake_clock));
    int result;
    do result=io.poll(io.context,fake_clock);while(result==PJ_POLL_WAITING);
    CHECK(result==PJ_POLL_READY);
    return io.reply(io.context,size);
}
static int push(const uint8_t *p,size_t n){return pj_live_feed(&player,p,n);}
/* Sample mode: what the app draws for the built-in sample (no art on the card, so every
 * card is a Jellyfin-style placeholder). Same renderer as the device. */
static pj_art_cache sample_art;
static uint16_t sample_frame[PJ_FB_W*PJ_FB_H];
static void sample_no_art(void) {
    for(unsigned i=0;i<library.count;i++)for(unsigned kind=0;kind<3;kind++) {
        pj_art_download none;memset(&none,0,sizeof(none));none.handle=library.items[i].handle;none.kind=kind;
        pj_art_store(&sample_art,&none);
    }
}
static void sample_screen(const char *folder,const char *name,int screen,unsigned selected) {
    pj_ui_state s={.screen=screen,.selected=selected,.library=&library,.art=&sample_art,.details=&details,
        .status="",.source="On this Pocket",.quality="",.can_stored=1,.can_tether=1};
    pj_ui_draw(sample_frame,&s);
    char path[512];snprintf(path,sizeof(path),"%s/%s.ppm",folder,name);
    FILE *f=fopen(path,"wb");CHECK(f);fprintf(f,"P6\n320 240\n255\n");
    for(unsigned i=0;i<PJ_FB_W*PJ_FB_H;i++) {
        unsigned c=sample_frame[i];
        fputc((int)(((c>>11)&31)*255/31),f);fputc((int)(((c>>5)&63)*255/63),f);fputc((int)((c&31)*255/31),f);
    }
    CHECK(!ferror(f));fclose(f);
}
/* Plays from start_seconds to the end; returns frames shown. */
static unsigned play(uint32_t handle,unsigned start_seconds,uint32_t *actual) {
    char body[128];size_t n;
    snprintf(body,sizeof(body),"{\"handle\":%lu,\"start_seconds\":%u}",(unsigned long)handle,start_seconds);
    const uint8_t *reply=ask(PJ_RPC_START,body,&n);
    pj_json j;pj_json_token tokens[16];uint64_t value;
    CHECK(pj_json_parse(&j,reply,n,tokens,16));
    CHECK(pj_json_u64(&j,pj_json_find(&j,0,"session"),&value));uint32_t session=(uint32_t)value;
    CHECK(pj_json_u64(&j,pj_json_find(&j,0,"start_seconds"),&value));*actual=(uint32_t)value;
    pj_live_ops ops={&model,audio_reset,audio_free,audio_write,video_begin,video_poll,video_abort,present};
    memset(&model,0,sizeof(model));pj_live_init(&player,ops);pj_stream_reader_init(&stream,session);
    static uint8_t chunk[PJ_SD_CHUNK];uint64_t offset=0;int eof=0;
    for(unsigned tick=0;tick<4000000&&!player.complete;tick++) {
        fake_clock+=1000;
        model.queued-=model.queued<48?model.queued:48;
        if(!eof) {
            snprintf(body,sizeof(body),"{\"session\":%lu,\"offset\":%llu}",(unsigned long)session,(unsigned long long)offset);
            reply=ask(PJ_RPC_READ,body,&n);
            CHECK(n>=16&&pj_av_u32(reply)==session&&pj_av_u64(reply+4)==offset);
            if(reply[12]==3){CHECK(pj_stream_reader_eof(&stream));eof=1;}
            else {
                CHECK(reply[12]==1&&n-16<=sizeof(chunk));
                size_t size=n-16,used=0;memcpy(chunk,reply+16,size);offset+=size;
                while(used<size) {
                    size_t consumed=0;
                    int result=pj_stream_reader_feed(&stream,chunk+used,size-used,&consumed,push);
                    CHECK(result>=0);used+=consumed;
                    if(!result)for(unsigned k=0;k<40;k++) {
                        fake_clock+=1000;model.queued-=model.queued<48?model.queued:48;
                        CHECK(pj_live_service(&player,fake_clock)>=0);
                    }
                }
            }
        }
        CHECK(pj_live_service(&player,fake_clock)>=0);
    }
    CHECK(player.complete);
    ask(PJ_RPC_STOP,"{\"session\":1}",&n);
    return model.presented;
}
int main(int argc,char **argv) {
    /* Optional: "sample" and a preview folder for the core's built-in sample library. */
    int sample=argc==8&&!strcmp(argv[6],"sample");
    CHECK(argc==6||sample);library_path=argv[1];pack_path=argv[2];
    unsigned expect_frames=(unsigned)strtoul(argv[3],0,10),fps=(unsigned)strtoul(argv[4],0,10);
    unsigned seek=(unsigned)strtoul(argv[5],0,10);
    CHECK(pj_sd_open(&io,&sd,clock_us)&&io.stored&&sd.title_count==1);
    size_t n;const uint8_t *reply=ask(PJ_RPC_HELLO,"{}",&n);
    CHECK(strstr((const char *)reply,"PJAV-JPEG-IMA"));
    reply=ask(PJ_RPC_HOME,"{}",&n);CHECK(pj_library_load(&library,reply,n,1));
    /* The card's titles open from an "On this Pocket" tile at the end of My Media, the
     * first row, not from a row of their own further down. */
    CHECK(library.row_count>=2&&!strcmp(library.rows[0].title,"My Media"));
    for(unsigned r=0;r<library.row_count;r++)CHECK(strcmp(library.rows[r].title,"On this Pocket"));
    const pj_library_item *tile=&library.items[library.rows[0].first+library.rows[0].count-1];
    CHECK(tile->folder&&!tile->unavailable&&!strcmp(tile->title,"On this Pocket"));
    int found_unavailable=0;
    for(unsigned i=0;i<library.count;i++)found_unavailable|=library.items[i].unavailable;
    CHECK(sample?!found_unavailable:found_unavailable);
    if(sample) {
        CHECK(library.row_count==2&&!strcmp(library.rows[1].title,"Latest Videos")&&library.rows[1].count==1);
        sample_no_art();sample_screen(argv[7],"ui-home-sample",PJ_SCREEN_HOME,library.rows[1].first);
    }
    char body[96];
    snprintf(body,sizeof(body),"{\"handle\":%lu,\"start\":0}",(unsigned long)tile->handle);
    reply=ask(PJ_RPC_BROWSE,body,&n);CHECK(pj_library_load(&library,reply,n,0));
    CHECK(library.count==1&&library.total==1&&library.next==1);
    const pj_library_item *title=&library.items[0];
    CHECK(!title->unavailable&&!title->folder&&library.rows[0].shape==PJ_ART_POSTER);
    snprintf(body,sizeof(body),"{\"handle\":%lu}",(unsigned long)title->handle);
    reply=ask(PJ_RPC_DETAILS,body,&n);CHECK(pj_details_load(&details,library.tokens,title->handle,reply,n));
    CHECK(details.overview[0]);
    if(sample) {
        sample_no_art();sample_screen(argv[7],"ui-details-sample",PJ_SCREEN_DETAILS,0);
        /* The sample carries no art at all: every request answers "none". */
        for(unsigned kind=0;kind<3;kind++) {
            snprintf(body,sizeof(body),"{\"handle\":%lu,\"kind\":%u,\"offset\":0}",(unsigned long)title->handle,kind);
            reply=ask(PJ_RPC_ART,body,&n);pj_art_begin(&art,title->handle,kind);
            CHECK(pj_art_chunk(&art,reply,n)&&art.complete&&!art.total);
        }
    }
    /* Multi-chunk hero art and a card, then an absent item. */
    for(unsigned kind=0;kind<3&&!sample;kind++) {
        pj_art_begin(&art,title->handle,kind);
        do {
            snprintf(body,sizeof(body),"{\"handle\":%lu,\"kind\":%u,\"offset\":%lu}",
                (unsigned long)title->handle,kind,(unsigned long)art.used);
            reply=ask(PJ_RPC_ART,body,&n);CHECK(pj_art_chunk(&art,reply,n));
        } while(!art.complete);
        CHECK(art.total==pj_art_width(kind)*pj_art_height(kind)*2);
        CHECK(art.bytes[0]==(uint8_t)(kind+1));
    }
    reply=ask(PJ_RPC_ART,"{\"handle\":999999,\"kind\":1,\"offset\":0}",&n);
    pj_art_begin(&art,999999,PJ_ART_POSTER);CHECK(pj_art_chunk(&art,reply,n)&&art.complete&&!art.total);
    /* A request for something not on the card is refused, not fatal. */
    CHECK(io.begin(io.context,PJ_RPC_START,"{\"handle\":999999}",fake_clock));
    CHECK(io.poll(io.context,fake_clock)==PJ_POLL_REFUSED&&io.idle(io.context));
    uint32_t handle=title->handle,actual=0;
    unsigned shown=play(handle,0,&actual);
    CHECK(!actual&&shown==expect_frames);
    uint64_t audio=model.written;
    /* Seek: mid-file start, timestamps rebased, no audio-gap failure, shorter run. */
    unsigned later=play(handle,seek,&actual);
    CHECK(actual==seek/PJ_SD_SEEK_SECONDS*PJ_SD_SEEK_SECONDS);
    unsigned expected_after=expect_frames-actual*fps;
    CHECK(later+2>=expected_after&&later<=expected_after+2);
    CHECK(model.written<audio);
    printf("PASS: on-card source: Home/details/art/refusal, full play %u frames %llu audio frames, seek to %us played %u frames. Modeled DAC, libjpeg header check, no hardware.\n",
        shown,(unsigned long long)audio,(unsigned)actual,later);
    return 0;
}
