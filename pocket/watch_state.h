/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Where the user stopped each title and which titles are watched, kept on this Pocket.
 *
 * Records are keyed by an identity of the title (title, subtitle, year, run time), not by
 * handle: the Chromatic numbers items afresh on every Home, and the same title seen over the
 * tether and from the PC-synced card gets the same record. The Pocket cannot tell the server
 * yet (no PJB1 progress request), so the local record wins over what the source says when
 * both exist. Stored in the head of pjcache0.sav (pocket/tether_store.h) and persisted by the
 * Pocket's save-on-exit. Numbers only: no titles are stored, only their 32-bit identity.
 */
#ifndef PJ_WATCH_STATE_H
#define PJ_WATCH_STATE_H
#include <stdint.h>
#include <string.h>
#include "live_library.h"
#define PJ_WATCH_MAX 512u
#define PJ_WATCH_RECORD 16u
#define PJ_WATCH_BYTES (16u+PJ_WATCH_MAX*PJ_WATCH_RECORD)   /* 8,208 */
#define PJ_WATCH_PLAYED_PERCENT 90u /* Jellyfin's default: past this, a stop counts as watched */
typedef struct {uint32_t key,position_s,stamp;uint8_t played;} pj_watch_entry;
typedef struct {pj_watch_entry entries[PJ_WATCH_MAX];unsigned count;uint32_t clock;int dirty;} pj_watch_state;

static inline uint32_t pj_watch_fnv(uint32_t h,const void *p,size_t n) {
    const uint8_t *b=p;for(size_t i=0;i<n;i++){h^=b[i];h*=16777619u;}return h;
}
static inline uint32_t pj_watch_key(const pj_library_item *item) {
    if(!item||item->folder)return 0;
    uint32_t h=2166136261u;
    h=pj_watch_fnv(h,item->title,strlen(item->title)+1);
    h=pj_watch_fnv(h,item->subtitle,strlen(item->subtitle)+1);
    uint8_t n[10];uint64_t d=item->duration_ticks;
    for(unsigned i=0;i<8;i++)n[i]=(uint8_t)(d>>(8*i));
    n[8]=(uint8_t)item->year;n[9]=(uint8_t)(item->year>>8);
    h=pj_watch_fnv(h,n,sizeof(n));
    return h?h:1u;
}
static inline pj_watch_entry *pj_watch_find(pj_watch_state *w,uint32_t key) {
    if(!key)return NULL;
    for(unsigned i=0;i<w->count;i++)if(w->entries[i].key==key)return &w->entries[i];
    return NULL;
}
/* Note a stop at `position_s` (or the end, `finished`). Returns the entry. Past 90% of the run
 * time counts as watched, like Jellyfin; a watched title resumes from the start. */
static inline pj_watch_entry *pj_watch_note(pj_watch_state *w,const pj_library_item *item,uint32_t position_s,int finished) {
    uint32_t key=pj_watch_key(item);if(!key)return NULL;
    pj_watch_entry *e=pj_watch_find(w,key);
    if(!e) {
        if(w->count<PJ_WATCH_MAX)e=&w->entries[w->count++];
        else { /* full: the record touched longest ago goes */
            e=&w->entries[0];
            for(unsigned i=1;i<w->count;i++)if(w->entries[i].stamp<e->stamp)e=&w->entries[i];
        }
        memset(e,0,sizeof(*e));e->key=key;
    }
    uint64_t duration=item->duration_ticks/10000000u;
    int played=finished||(duration&&(uint64_t)position_s*100u>=duration*PJ_WATCH_PLAYED_PERCENT);
    e->played=(uint8_t)played;e->position_s=played?0:position_s;e->stamp=++w->clock;w->dirty=1;
    return e;
}
/* Overlay the local records on a freshly loaded Home or page. Returns how many changed. */
static inline unsigned pj_watch_apply(pj_watch_state *w,pj_live_library *m) {
    unsigned changed=0;
    for(unsigned i=0;i<m->count;i++) {
        pj_library_item *item=&m->items[i];
        pj_watch_entry *e=pj_watch_find(w,pj_watch_key(item));
        if(!e)continue;
        uint64_t resume=(uint64_t)e->position_s*10000000u;
        if(item->resume_ticks!=resume||item->played!=e->played)changed++;
        item->resume_ticks=resume;item->played=e->played;
    }
    return changed;
}
/* "PJWS", version 1, count, clock, then 16-byte records. */
static inline size_t pj_watch_save(const pj_watch_state *w,uint8_t *out,size_t capacity) {
    size_t size=16u+w->count*PJ_WATCH_RECORD;
    if(capacity<size)return 0;
    memcpy(out,"PJWS",4);pj_put32(out+4,1);pj_put32(out+8,w->count);pj_put32(out+12,w->clock);
    for(unsigned i=0;i<w->count;i++) {
        uint8_t *r=out+16+i*PJ_WATCH_RECORD;const pj_watch_entry *e=&w->entries[i];
        pj_put32(r,e->key);pj_put32(r+4,e->position_s);pj_put32(r+8,e->stamp);pj_put32(r+12,e->played);
    }
    return size;
}
static inline int pj_watch_load(pj_watch_state *w,const uint8_t *in,size_t size) {
    memset(w,0,sizeof(*w));
    if(size<16||memcmp(in,"PJWS",4)||pj_av_u32(in+4)!=1)return 0;
    uint32_t count=pj_av_u32(in+8);
    if(count>PJ_WATCH_MAX||size<16u+count*PJ_WATCH_RECORD)return 0;
    for(unsigned i=0;i<count;i++) {
        const uint8_t *r=in+16+i*PJ_WATCH_RECORD;pj_watch_entry *e=&w->entries[i];
        e->key=pj_av_u32(r);e->position_s=pj_av_u32(r+4);e->stamp=pj_av_u32(r+8);e->played=(uint8_t)(pj_av_u32(r+12)!=0);
        if(!e->key||e->position_s>172800u){memset(w,0,sizeof(*w));return 0;}
    }
    w->count=count;w->clock=pj_av_u32(in+12);return 1;
}
#endif
