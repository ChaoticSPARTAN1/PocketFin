/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Background library sync over the tether (Jellymatic's Pocket link docs, phase 2): after
 * Home arrives, walk every Home tile once and build a fresh saved image (tether_cache.h):
 *
 *   tile picture (row shape)  -> reuse by art key from the last saved image, else ART
 *   details (videos)          -> reuse by title identity, else DETAILS
 *   first folder page         -> BROWSE start 0, always fresh (its handles belong to this Home)
 *   backdrop                  -> reuse by hero key, else ART, only while room is left for
 *                                the remaining tiles (the cards always win)
 *
 * So a re-sync where nothing changed sends no ART at all, and one replaced movie costs one
 * picture. Never blocks: pj_sync_next hands out one request at a time (or does a bounded
 * number of copies), the app sends it whenever its own requests leave the link idle, and
 * pj_sync_reply takes the answer. If the cable is pulled, pj_sync_offline finishes the image
 * from what is already saved, so the next start still shows the newest Home.
 */
#ifndef PJ_TETHER_SYNC_H
#define PJ_TETHER_SYNC_H
#include "live_library.h"
#include "tether_cache.h"
#include "watch_state.h"
#define PJ_SYNC_COPIES 24u          /* copies per pj_sync_next call, so a pass stays short */
#define PJ_SYNC_DETAILS_RESERVE 2048u
enum {PJ_SY_IDLE,PJ_SY_RUNNING,PJ_SY_DONE};
enum {PJ_SY_CARD,PJ_SY_TEXT,PJ_SY_HERO,PJ_SY_NEXT_ITEM};
#define PJ_SY_FINISHED 0
#define PJ_SY_REQUEST 1
#define PJ_SY_MORE 2
typedef struct {
    pj_tc_image *next;const pj_tc_image *prev;
    pj_live_library home;          /* the Home being saved (the app's own model changes on browse) */
    pj_art_download art;
    int state,art_keys,offline,waiting,full;
    unsigned item,step,done,total,command;char body[96];
    unsigned fetched,reused,skipped,requests,heroes,fresh;uint32_t fetched_bytes;
} pj_sync;

static inline uint32_t pj_sync_folder_key(const pj_library_item *item) {
    uint32_t h=pj_watch_fnv(2166136261u,"folder",6);
    h=pj_watch_fnv(h,item->title,strlen(item->title));return h?h:1u;
}
static inline unsigned pj_sync_shape(const pj_live_library *m,unsigned item) {
    for(unsigned r=0;r<m->row_count;r++)
        if(item>=m->rows[r].first&&item<m->rows[r].first+m->rows[r].count)return m->rows[r].shape;
    return PJ_ART_POSTER;
}
/* `next` must be pj_tc_init'ed; `prev` (may be NULL or not ready) is the image saved before. */
static inline int pj_sync_begin(pj_sync *s,pj_tc_image *next,const pj_tc_image *prev,const uint8_t *home,size_t size,int art_keys) {
    s->state=PJ_SY_IDLE;s->next=next;s->prev=prev&&prev->ready?prev:NULL;
    s->art_keys=art_keys;s->offline=s->waiting=s->full=0;s->item=s->step=s->done=0;
    s->fetched=s->reused=s->skipped=s->requests=s->heroes=s->fresh=0;s->fetched_bytes=0;s->art.handle=0;
    if(!pj_library_load(&s->home,home,size,1))return 0;
    if(!pj_tc_add(next,PJ_RPC_HOME,0,0,0,0,home,size))return 0;
    s->total=s->home.count;s->state=PJ_SY_RUNNING;return 1;
}
static inline void pj_sync_offline(pj_sync *s){s->offline=1;s->waiting=0;}
static inline const pj_tc_record *pj_sync_known(const pj_sync *s,unsigned command,unsigned kind,uint32_t key) {
    const pj_tc_record *r=pj_tc_find_key(s->next,command,kind,key);
    if(!r&&s->prev)r=pj_tc_find_key(s->prev,command,kind,key);
    return r;
}
/* Copy a known blob under this tile's handle. One already in the new image is shared, not copied. */
static inline int pj_sync_reuse(pj_sync *s,const pj_tc_record *r,unsigned command,unsigned kind,uint32_t handle,uint32_t key) {
    if(r>=s->next->records&&r<s->next->records+s->next->count) {
        if(s->next->count>=PJ_TC_RECORDS)return 0;
        pj_tc_record *d=&s->next->records[s->next->count++];
        *d=*r;d->command=(uint8_t)command;d->kind=(uint8_t)kind;d->handle=handle;d->start=0;d->key=key;return 1;
    }
    return pj_tc_add(s->next,command,kind,handle,0,key,pj_tc_blob(s->prev,r),r->size);
}
/* Room the remaining tiles' cards and details still need, so backdrops never crowd them out. */
static inline size_t pj_sync_reserve(const pj_sync *s) {
    unsigned left=s->total>s->item+1?s->total-s->item-1:0;
    return (size_t)left*(PJ_ART_POSTER_W*PJ_ART_POSTER_H*2u+PJ_SYNC_DETAILS_RESERVE);
}
static inline void pj_sync_advance(pj_sync *s) {
    if(++s->step>=PJ_SY_NEXT_ITEM){s->step=PJ_SY_CARD;s->item++;s->done++;}
}
/* One step of the current tile: 1 a request is ready in command/body, 0 the step is settled. */
static inline int pj_sync_step(pj_sync *s) {
    const pj_library_item *it=&s->home.items[s->item];
    uint32_t handle=it->handle;
    if(s->step==PJ_SY_CARD||s->step==PJ_SY_HERO) {
        unsigned kind=s->step==PJ_SY_CARD?pj_sync_shape(&s->home,s->item):PJ_ART_BACKDROP;
        uint32_t key=s->step==PJ_SY_CARD?it->art_key:it->hero_key;
        size_t bytes=(size_t)pj_art_width(kind)*pj_art_height(kind)*2u;
        if(pj_tc_find(s->next,PJ_RPC_ART,kind,handle,0))return 0;          /* same tile in another row */
        if(s->art_keys&&!key)return 0;                                       /* no picture to show */
        if(s->step==PJ_SY_HERO&&(it->folder||pj_tc_room(s->next)<bytes+pj_sync_reserve(s))){s->skipped++;return 0;}
        if(s->full)return 0;
        const pj_tc_record *r=s->art_keys?pj_sync_known(s,PJ_RPC_ART,kind,key):NULL;
        if(r) {
            if(!pj_sync_reuse(s,r,PJ_RPC_ART,kind,handle,key))s->full=1;
            else{s->reused++;if(kind==PJ_ART_BACKDROP)s->heroes++;}
            return 0;
        }
        if(s->offline){s->skipped++;return 0;}
        if(s->art.handle!=handle||s->art.kind!=kind)pj_art_begin(&s->art,handle,kind);
        s->command=PJ_RPC_ART;
        snprintf(s->body,sizeof(s->body),"{\"handle\":%lu,\"kind\":%u,\"offset\":%lu}",(unsigned long)handle,kind,(unsigned long)s->art.used);
        return 1;
    }
    /* PJ_SY_TEXT: details for a video, the first page for a folder */
    if(it->folder) {
        if(pj_tc_find(s->next,PJ_RPC_BROWSE,0,handle,0)||s->offline||s->full)return 0;
        s->command=PJ_RPC_BROWSE;snprintf(s->body,sizeof(s->body),"{\"handle\":%lu,\"start\":0}",(unsigned long)handle);
        return 1;
    }
    if(pj_tc_find(s->next,PJ_RPC_DETAILS,0,handle,0)||s->full)return 0;
    uint32_t key=pj_watch_key(it);
    const pj_tc_record *r=pj_sync_known(s,PJ_RPC_DETAILS,0,key);
    if(r) {
        if(!pj_sync_reuse(s,r,PJ_RPC_DETAILS,0,handle,key))s->full=1;else s->reused++;
        return 0;
    }
    if(s->offline)return 0;
    s->command=PJ_RPC_DETAILS;snprintf(s->body,sizeof(s->body),"{\"handle\":%lu}",(unsigned long)handle);
    return 1;
}
/* PJ_SY_REQUEST: send s->command / s->body, then pj_sync_reply. PJ_SY_MORE: call again on a
 * later pass. PJ_SY_FINISHED: the new image is finished (s->next->ready). */
static inline int pj_sync_next(pj_sync *s) {
    if(s->state!=PJ_SY_RUNNING)return PJ_SY_FINISHED;
    if(s->waiting)return PJ_SY_MORE;
    for(unsigned work=0;work<PJ_SYNC_COPIES;work++) {
        if(s->item>=s->home.count) {
            pj_tc_finish(s->next);s->state=PJ_SY_DONE;return PJ_SY_FINISHED;
        }
        if(pj_sync_step(s)){s->waiting=1;s->requests++;return PJ_SY_REQUEST;}
        pj_sync_advance(s);
    }
    return PJ_SY_MORE;
}
/* The answer to the last request (a PJ_POLL_* result). Returns 2 when an artwork download
 * finished (s->art holds it, for the screen), 1 otherwise. A failure leaves the step to be sent
 * again, or skipped once pj_sync_offline was called. */
static inline int pj_sync_reply(pj_sync *s,int result,const uint8_t *reply,size_t size) {
    if(!s->waiting)return 1;
    s->waiting=0;
    if(result==PJ_POLL_FAILED)return 1;
    const pj_library_item *it=&s->home.items[s->item];
    if(s->command==PJ_RPC_ART) {
        if(result!=PJ_POLL_READY||!pj_art_chunk(&s->art,reply,size)){s->art.handle=0;pj_sync_advance(s);return 1;}
        if(!s->art.complete)return 1; /* the next chunk on the next call */
        unsigned kind=s->art.kind;uint32_t key=kind==PJ_ART_BACKDROP?it->hero_key:it->art_key;
        s->fetched++;s->fetched_bytes+=s->art.total;if(s->art.total)s->fresh++; /* a new picture */
        if(s->art.total&&!pj_tc_add(s->next,PJ_RPC_ART,kind,s->art.handle,0,key,s->art.bytes,s->art.total))s->full=1;
        else if(s->art.total&&kind==PJ_ART_BACKDROP)s->heroes++;
        pj_sync_advance(s);return 2;
    }
    if(result==PJ_POLL_READY) {
        s->fetched++;s->fetched_bytes+=(uint32_t)size;
        if(s->command==PJ_RPC_DETAILS) {
            s->fresh++;
            if(!pj_tc_add(s->next,PJ_RPC_DETAILS,0,it->handle,0,pj_watch_key(it),reply,size))s->full=1;
        } else if(!pj_tc_add(s->next,PJ_RPC_BROWSE,0,it->handle,0,pj_sync_folder_key(it),reply,size))s->full=1;
    }
    pj_sync_advance(s);return 1;
}
/* Artwork the app fetched for the screen while the sync runs: keep it, so the sync does not
 * ask for it again. Only pictures of this Home's tiles (with the key they were shown under). */
static inline void pj_sync_offer(pj_sync *s,const pj_art_download *a) {
    if(s->state!=PJ_SY_RUNNING||!a->complete||!a->total||s->full)return;
    for(unsigned i=0;i<s->home.count;i++) {
        const pj_library_item *it=&s->home.items[i];
        if(it->handle!=a->handle)continue;
        uint32_t key=0;
        if(a->kind==PJ_ART_BACKDROP)key=it->hero_key;
        else if(pj_sync_shape(&s->home,i)==a->kind)key=it->art_key;
        else continue;
        if(a->kind==PJ_ART_BACKDROP&&pj_tc_room(s->next)<a->total+pj_sync_reserve(s))return;
        if(pj_tc_add(s->next,PJ_RPC_ART,a->kind,a->handle,0,key,a->bytes,a->total)&&a->kind==PJ_ART_BACKDROP)s->heroes++;
        return;
    }
}
#endif
