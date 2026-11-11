/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Keeps the saved Chromatic library and the watch state on this Pocket's card.
 *
 * Slot map (tools/package_release.py declares them, nonvolatile, 256 KiB each, in the
 * CRAM0 save window of the SDK template):
 *   10        pjlive_log.sav   diagnostics log (diag_log.h)
 *   11        pjcache0.sav     store header (64 bytes) + watch state (watch_state.h)
 *   12 .. 19  pjcache1..8.sav  the saved library image (tether_cache.h), 2 MiB, in order
 *
 * How it reaches the card (Jellymatic's Pocket link docs "APF SD write-back"): the app writes
 * the slots with ordinary fopen("slot:N")/fwrite. That lands in CRAM0 at once; the Pocket
 * copies the save window to the .sav files when the core is quit (save-on-exit). The SDK's app
 * interface has no runtime "write this slot to SD now" call (of_file_slot_write_chunked is a
 * kernel HAL function, not in the app ABI, and the upstream SDK stays unmodified), so the
 * optional runtime flush is not used; quitting from the Analogue menu is what commits it.
 *
 * Writes are chunked (one 32 KiB piece per app pass, only while nothing plays and no card read
 * is in flight, because CRAM0 is shared with card reads), so the UI keeps running. A piece
 * that takes longer than PJ_STORE_STALL_US abandons the pass; the next change tries again.
 * Order: header marked "image being written", image slots, header with the image's size and
 * CRC. An interrupted pass therefore never leaves a header that vouches for a half image.
 */
#ifndef PJ_TETHER_STORE_H
#define PJ_TETHER_STORE_H
#include <stdio.h>
#include <string.h>
#include "tether_cache.h"
#include "watch_state.h"
#define PJ_STORE_HEAD 64u
#define PJ_STORE_STATE_SLOT PJ_TETHER_SLOT_FIRST                   /* 11 */
#define PJ_STORE_IMAGE_SLOT (PJ_TETHER_SLOT_FIRST+1u)              /* 12 */
#define PJ_STORE_CHUNK 32768u
#define PJ_STORE_STALL_US 250000u
enum {PJ_ST_IDLE,PJ_ST_INVALIDATE,PJ_ST_IMAGE,PJ_ST_FINAL};

#if defined(OF_PC)
/* Desktop checks: slots are files <dir>/slotNN.sav when a directory is set, else nothing. */
static const char *pj_store_pc_dir;
static inline FILE *pj_store_fopen(unsigned slot,const char *mode) {
    if(!pj_store_pc_dir)return NULL;
    char path[512];snprintf(path,sizeof(path),"%s/slot%02u.sav",pj_store_pc_dir,slot);return fopen(path,mode);
}
#else
static inline FILE *pj_store_fopen(unsigned slot,const char *mode) {
    char path[16];snprintf(path,sizeof(path),"slot:%u",slot);return fopen(path,mode);
}
#endif

typedef struct {
    int phase,failed;const char *why;
    const uint8_t *image;uint32_t image_size,image_crc,generation;
    int image_pending,state_pending;
    unsigned slot;uint32_t at;   /* image slot being written, bytes already in it */
    unsigned pieces;uint32_t worst_us;
    uint32_t card_size,card_crc; /* the image the card's header may vouch for (0: none) */
    uint32_t settings; /* 2026-11-10: the merged Core Settings word (pocket/core_settings.h), header bytes 28-31; 0 none */
    uint8_t head[PJ_STORE_HEAD+PJ_WATCH_BYTES];
} pj_store;

static inline uint32_t pj_store_crc(const uint8_t *p,size_t n){return pj_av_crc_part(0xffffffffu,p,n)^0xffffffffu;}
/* Header: "PJTC", version 1, image size, image CRC, generation, state size, state CRC, then (2026-11-10, in what
 * was zero: an older app reads it as nothing) the Core Settings word and its check (the word's bitwise NOT). */
static inline size_t pj_store_build_head(pj_store *st,const pj_watch_state *w,int with_image) {
    memset(st->head,0,PJ_STORE_HEAD);
    size_t state=pj_watch_save(w,st->head+PJ_STORE_HEAD,PJ_WATCH_BYTES);
    memcpy(st->head,"PJTC",4);pj_put32(st->head+4,1);
    pj_put32(st->head+8,with_image?st->card_size:0);pj_put32(st->head+12,with_image?st->card_crc:0);
    pj_put32(st->head+16,st->generation);pj_put32(st->head+20,(uint32_t)state);
    pj_put32(st->head+24,pj_store_crc(st->head+PJ_STORE_HEAD,state));
    if(st->settings){pj_put32(st->head+28,st->settings);pj_put32(st->head+32,~st->settings);}
    return PJ_STORE_HEAD+state;
}
/* One whole small file or one piece of an image slot. 1 written. */
static inline int pj_store_put(pj_store *st,unsigned slot,const char *mode,const void *data,size_t n) {
    FILE *f=pj_store_fopen(slot,mode);
    if(!f){st->why="cannot open a cache slot";return 0;}
    int ok=!n||fwrite(data,1,n,f)==n;
    if(fclose(f)!=0)ok=0;
    if(!ok)st->why="cannot write a cache slot";
    return ok;
}
/* Startup: watch state always, the image when its CRC holds. Returns 1 when an image loaded. */
static inline int pj_store_load(pj_store *st,pj_watch_state *w,pj_tc_image *image,uint8_t *buffer,size_t capacity) {
    memset(st,0,sizeof(*st));memset(w,0,sizeof(*w));pj_tc_init(image,buffer,capacity);
    FILE *f=pj_store_fopen(PJ_STORE_STATE_SLOT,"rb");
    if(!f){st->why="no saved state yet";return 0;}
    size_t n=fread(st->head,1,sizeof(st->head),f);fclose(f);
    if(n<PJ_STORE_HEAD||memcmp(st->head,"PJTC",4)||pj_av_u32(st->head+4)!=1){st->why="no saved state yet";return 0;}
    if(pj_av_u32(st->head+28)&&pj_av_u32(st->head+32)==~pj_av_u32(st->head+28))st->settings=pj_av_u32(st->head+28);
    uint32_t state=pj_av_u32(st->head+20);
    if(state<=PJ_WATCH_BYTES&&PJ_STORE_HEAD+state<=n&&pj_store_crc(st->head+PJ_STORE_HEAD,state)==pj_av_u32(st->head+24))
        pj_watch_load(w,st->head+PJ_STORE_HEAD,state);
    st->generation=pj_av_u32(st->head+16);
    uint32_t size=pj_av_u32(st->head+8),crc=pj_av_u32(st->head+12);
    if(!size){st->why="no saved library yet";return 0;}
    if(size>capacity){st->why="saved library damaged";return 0;}
    for(uint32_t at=0,slot=PJ_STORE_IMAGE_SLOT;at<size;slot++) {
        uint32_t want=size-at;if(want>PJ_TETHER_SLOT_BYTES)want=PJ_TETHER_SLOT_BYTES;
        f=pj_store_fopen(slot,"rb");
        if(!f){st->why="saved library incomplete";return 0;}
        n=fread(buffer+at,1,want,f);fclose(f);
        if(n!=want){st->why="saved library incomplete";return 0;}
        at+=want;
    }
    if(pj_store_crc(buffer,size)!=crc||!pj_tc_load(image,buffer,capacity,size)){
        pj_tc_init(image,buffer,capacity);st->why="saved library damaged";return 0;
    }
    st->card_size=size;st->card_crc=crc;st->why="";return 1;
}
static inline void pj_store_save_image(pj_store *st,const pj_tc_image *image) {
    st->image=image->data;st->image_size=(uint32_t)image->used;st->image_crc=pj_store_crc(image->data,image->used);
    st->generation++;st->image_pending=1;st->state_pending=1;st->failed=0;
    st->phase=PJ_ST_INVALIDATE;
}
/* The watch state changed. After an abandoned image pass the whole image goes again. */
static inline void pj_store_save_state(pj_store *st) {
    st->state_pending=1;st->failed=0;
    if(st->phase==PJ_ST_IDLE)st->phase=st->image_pending?PJ_ST_INVALIDATE:PJ_ST_FINAL;
}
static inline int pj_store_busy(const pj_store *st){return st->phase!=PJ_ST_IDLE;}
static inline void pj_store_abandon(pj_store *st,const char *why) {
    /* Mid-image the card no longer holds a whole image: its header must not vouch for one. */
    if(st->phase==PJ_ST_INVALIDATE||st->phase==PJ_ST_IMAGE){st->card_size=0;st->card_crc=0;}
    st->phase=PJ_ST_IDLE;st->failed=1;st->why=why;
}
/* One bounded piece of work. 1 a piece was written, 0 nothing to do, -1 failed/abandoned
 * (st->why). The caller only calls this while nothing plays and the card is quiet. */
static inline int pj_store_step(pj_store *st,pj_watch_state *w,uint32_t (*clock_us)(void)) {
    if(st->phase==PJ_ST_IDLE)return 0;
    uint32_t began=clock_us();int ok=1;
    if(st->phase==PJ_ST_INVALIDATE) {
        size_t n=pj_store_build_head(st,w,0);
        ok=pj_store_put(st,PJ_STORE_STATE_SLOT,"wb",st->head,n);
        st->phase=PJ_ST_IMAGE;st->slot=PJ_STORE_IMAGE_SLOT;st->at=0;
    } else if(st->phase==PJ_ST_IMAGE) {
        uint32_t base=(st->slot-PJ_STORE_IMAGE_SLOT)*PJ_TETHER_SLOT_BYTES;
        uint32_t in_slot=base<st->image_size?st->image_size-base:0;
        if(in_slot>PJ_TETHER_SLOT_BYTES)in_slot=PJ_TETHER_SLOT_BYTES;
        uint32_t n=in_slot-st->at;if(n>PJ_STORE_CHUNK)n=PJ_STORE_CHUNK;
        /* The first piece truncates the slot ("wb"); an unused slot is left empty. */
        ok=pj_store_put(st,st->slot,st->at?"ab":"wb",st->image+base+st->at,n);
        st->at+=n;
        if(st->at>=in_slot) {
            st->slot++;st->at=0;
            if(st->slot>=PJ_STORE_IMAGE_SLOT+PJ_TC_IMAGE_SLOTS)st->phase=PJ_ST_FINAL;
        }
    } else {
        if(st->image_pending){st->card_size=st->image_size;st->card_crc=st->image_crc;} /* every slot is written */
        size_t n=pj_store_build_head(st,w,st->card_size!=0);
        ok=pj_store_put(st,PJ_STORE_STATE_SLOT,"wb",st->head,n);
        if(ok){st->phase=PJ_ST_IDLE;st->state_pending=0;w->dirty=0;st->image_pending=0;}
    }
    uint32_t took=clock_us()-began;if(took>st->worst_us)st->worst_us=took;
    st->pieces++;
    if(!ok){pj_store_abandon(st,st->why?st->why:"cannot write a cache slot");return -1;}
    if(took>PJ_STORE_STALL_US){pj_store_abandon(st,"the card was too slow; saving again later");return -1;}
    return 1;
}
#endif
