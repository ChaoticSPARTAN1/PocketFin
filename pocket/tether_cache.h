/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * The Pocket's saved copy of what the Chromatic showed it (Jellymatic's Pocket link docs,
 * phase 2): Home, details, the first page of each folder and the artwork, as one PJLB v1
 * library image in RAM, in the exact layout pocket/sd_library.h reads and
 * tools/pocket_library.py writes (header, record table, no titles, blobs). Each record's
 * reserved word holds the tether art key (or an item identity key for details/folders), so a
 * later sync can reuse a picture whatever handle the Chromatic gives that item next time.
 *
 * pj_tc_source serves the image as a pj_transport, answering the same PJB1 requests as the
 * tether and the PC library, so offline screens are the same screens. It has no video: START
 * is refused and the app shows the titles dimmed. Persistence is pocket/tether_store.h.
 */
#ifndef PJ_TETHER_CACHE_H
#define PJ_TETHER_CACHE_H
#include <stdio.h>
#include <string.h>
#include "../shared/bounded_json.h"
#include "../shared/bridge_rpc.h"
#include "../shared/pocket_tether.h"
#include "transport.h"
/* Slot 11 (pjcache0.sav) holds the store header and the watch state; the image fills the
 * eight slots after it (pjcache1.sav .. pjcache8.sav). */
#define PJ_TC_IMAGE_SLOTS (PJ_TETHER_SLOTS-1u)
#define PJ_TC_IMAGE_BYTES (PJ_TC_IMAGE_SLOTS*PJ_TETHER_SLOT_BYTES)   /* 2 MiB */
#define PJ_TC_RECORDS 512u
#define PJ_TC_HEADER 32u
#define PJ_TC_RECORD 24u
#define PJ_TC_BLOBS (PJ_TC_HEADER+PJ_TC_RECORDS*PJ_TC_RECORD)
typedef struct {uint8_t command,kind;uint32_t handle,start,key,offset,size;} pj_tc_record;
typedef struct {
    uint8_t *data;size_t capacity,used;
    pj_tc_record records[PJ_TC_RECORDS];unsigned count;
    int ready; /* 1: a finished image (header written), data[0..used) */
} pj_tc_image;

static inline void pj_tc_init(pj_tc_image *c,uint8_t *buffer,size_t capacity) {
    memset(c,0,sizeof(*c));c->data=buffer;c->capacity=capacity;c->used=PJ_TC_BLOBS;
}
static inline const pj_tc_record *pj_tc_find(const pj_tc_image *c,unsigned command,unsigned kind,uint32_t handle,uint32_t start) {
    for(unsigned i=0;i<c->count;i++) {
        const pj_tc_record *r=&c->records[i];
        if(r->command==command&&r->kind==kind&&r->handle==handle&&r->start==start)return r;
    }
    return NULL;
}
static inline const pj_tc_record *pj_tc_find_key(const pj_tc_image *c,unsigned command,unsigned kind,uint32_t key) {
    if(!key)return NULL;
    for(unsigned i=0;i<c->count;i++) {
        const pj_tc_record *r=&c->records[i];
        if(r->command==command&&r->kind==kind&&r->key==key)return r;
    }
    return NULL;
}
static inline const uint8_t *pj_tc_blob(const pj_tc_image *c,const pj_tc_record *r){return c->data+r->offset;}
/* 1 stored (or already there), 0 no room. */
static inline int pj_tc_add(pj_tc_image *c,unsigned command,unsigned kind,uint32_t handle,uint32_t start,uint32_t key,
                            const uint8_t *data,size_t size) {
    if(c->ready)return 0;
    if(pj_tc_find(c,command,kind,handle,start))return 1;
    if(c->count>=PJ_TC_RECORDS||size>c->capacity-c->used)return 0;
    pj_tc_record *r=&c->records[c->count++];
    r->command=(uint8_t)command;r->kind=(uint8_t)kind;r->handle=handle;r->start=start;r->key=key;
    r->offset=(uint32_t)c->used;r->size=(uint32_t)size;
    if(size)memmove(c->data+c->used,data,size);
    c->used+=size;return 1;
}
static inline size_t pj_tc_room(const pj_tc_image *c){return c->capacity-c->used;}
/* Header + record table into the reserved space; the blobs are already in place. */
static inline void pj_tc_finish(pj_tc_image *c) {
    uint8_t *h=c->data;uint32_t record_at=PJ_TC_HEADER,title_at=record_at+c->count*PJ_TC_RECORD;
    memset(h,0,PJ_TC_BLOBS);
    for(unsigned i=0;i<c->count;i++) {
        uint8_t *r=h+record_at+i*PJ_TC_RECORD;const pj_tc_record *s=&c->records[i];
        r[0]=s->command;r[1]=s->kind;pj_put32(r+4,s->handle);pj_put32(r+8,s->start);
        pj_put32(r+12,s->offset);pj_put32(r+16,s->size);pj_put32(r+20,s->key);
    }
    uint32_t crc=pj_av_crc_part(0xffffffffu,h+record_at,c->count*PJ_TC_RECORD)^0xffffffffu; /* no titles */
    memcpy(h,"PJLB",4);pj_put32(h+4,1);pj_put32(h+8,c->count);pj_put32(h+12,record_at);pj_put32(h+16,0);
    pj_put32(h+20,title_at);pj_put32(h+24,(uint32_t)c->used);pj_put32(h+28,crc);
    c->ready=1;
}
/* An image read back from the card (data already holds `size` bytes). 1 usable. */
static inline int pj_tc_load(pj_tc_image *c,uint8_t *buffer,size_t capacity,size_t size) {
    pj_tc_init(c,buffer,capacity);
    if(size<PJ_TC_BLOBS||size>capacity)return 0;
    const uint8_t *h=buffer;
    uint32_t records=pj_av_u32(h+8),record_at=pj_av_u32(h+12),titles=pj_av_u32(h+16);
    if(memcmp(h,"PJLB",4)||pj_av_u32(h+4)!=1||records>PJ_TC_RECORDS||record_at!=PJ_TC_HEADER||titles||
       pj_av_u32(h+20)!=record_at+records*PJ_TC_RECORD||pj_av_u32(h+24)!=size)return 0;
    if((pj_av_crc_part(0xffffffffu,h+record_at,records*PJ_TC_RECORD)^0xffffffffu)!=pj_av_u32(h+28))return 0;
    for(unsigned i=0;i<records;i++) {
        const uint8_t *r=h+record_at+i*PJ_TC_RECORD;pj_tc_record *d=&c->records[i];
        d->command=r[0];d->kind=r[1];d->handle=pj_av_u32(r+4);d->start=pj_av_u32(r+8);
        d->offset=pj_av_u32(r+12);d->size=pj_av_u32(r+16);d->key=pj_av_u32(r+20);
        uint32_t limit=d->command==PJ_RPC_ART?PJ_ART_BACKDROP_W*PJ_ART_BACKDROP_H*2u:PJ_RPC_LIMIT; /* a backdrop is 76,800 */
        if(d->offset<PJ_TC_BLOBS||d->offset>size||d->size>size-d->offset||d->size>limit)return 0;
    }
    c->count=records;c->used=size;c->ready=1;return 1;
}
static inline unsigned pj_tc_count(const pj_tc_image *c,unsigned command) {
    unsigned n=0;for(unsigned i=0;i<c->count;i++)n+=c->records[i].command==command;return n;
}

/* ---- the saved library as a source ---- */
typedef struct {const pj_tc_image *image;uint8_t reply[PJ_RPC_LIMIT+16];size_t reply_size;int state;} pj_tc_source;
enum {PJ_TCS_IDLE,PJ_TCS_READY,PJ_TCS_REFUSED,PJ_TCS_FAILED};
static int pj_tcs_idle(void *c){return ((pj_tc_source *)c)->state==PJ_TCS_IDLE;}
static inline int pj_tcs_copy(pj_tc_source *s,const pj_tc_record *r) {
    if(!r||r->size>PJ_RPC_LIMIT){s->state=PJ_TCS_REFUSED;return 1;}
    memcpy(s->reply,pj_tc_blob(s->image,r),r->size);s->reply_size=r->size;s->state=PJ_TCS_READY;return 1;
}
static int pj_tcs_begin(void *c,unsigned command,const char *json,uint32_t now) {
    pj_tc_source *s=c;(void)now;
    if(s->state!=PJ_TCS_IDLE||!s->image||!s->image->ready)return 0;
    pj_json_token tokens[32];pj_json j;uint64_t handle=0,value=0,kind=0,offset=0;
    if(!pj_json_parse(&j,(const uint8_t *)json,strlen(json),tokens,32))return 0;
    int at=pj_json_find(&j,0,"handle");if(at>=0&&!pj_json_u64(&j,at,&handle))return 0;
    if(handle>0xffffffffu)return 0;
    switch(command) {
    case PJ_RPC_HELLO: {
        static const char hello[]="{\"version\":1,\"transport\":\"tether-cache\",\"compressed_stream\":\"PJAV-JPEG-IMA\"}";
        memcpy(s->reply,hello,sizeof(hello)-1);s->reply_size=sizeof(hello)-1;s->state=PJ_TCS_READY;return 1;
    }
    case PJ_RPC_HOME:return pj_tcs_copy(s,pj_tc_find(s->image,PJ_RPC_HOME,0,0,0));
    case PJ_RPC_BROWSE:
        at=pj_json_find(&j,0,"start");if(at>=0&&!pj_json_u64(&j,at,&value))return 0;
        return pj_tcs_copy(s,value>0xffffffffu?NULL:pj_tc_find(s->image,PJ_RPC_BROWSE,0,(uint32_t)handle,(uint32_t)value));
    case PJ_RPC_DETAILS:return pj_tcs_copy(s,pj_tc_find(s->image,PJ_RPC_DETAILS,0,(uint32_t)handle,0));
    case PJ_RPC_ART: {
        at=pj_json_find(&j,0,"kind");if(at>=0&&!pj_json_u64(&j,at,&kind))return 0;
        at=pj_json_find(&j,0,"offset");if(at>=0&&!pj_json_u64(&j,at,&offset))return 0;
        if(kind>PJ_ART_BACKDROP)return 0;
        const pj_tc_record *r=pj_tc_find(s->image,PJ_RPC_ART,(unsigned)kind,(uint32_t)handle,0);
        uint32_t total=r?r->size:0;
        if(offset>total)return 0;
        uint32_t size=total-(uint32_t)offset;if(size>PJ_RPC_LIMIT-8)size=PJ_RPC_LIMIT-8;
        pj_put32(s->reply,total);
        unsigned w=pj_art_width((unsigned)kind),h=pj_art_height((unsigned)kind);
        s->reply[4]=(uint8_t)w;s->reply[5]=(uint8_t)(w>>8);s->reply[6]=(uint8_t)h;s->reply[7]=(uint8_t)(h>>8);
        if(size)memcpy(s->reply+8,pj_tc_blob(s->image,r)+offset,size);
        s->reply_size=8+size;s->state=PJ_TCS_READY;return 1;
    }
    case PJ_RPC_STOP: {
        static const char stopped[]="{\"stopped\":true}";
        memcpy(s->reply,stopped,sizeof(stopped)-1);s->reply_size=sizeof(stopped)-1;s->state=PJ_TCS_READY;return 1;
    }
    case PJ_RPC_START:s->state=PJ_TCS_REFUSED;return 1; /* no video in the saved library */
    default:return 0;
    }
}
static int pj_tcs_poll(void *c,uint32_t now) {
    pj_tc_source *s=c;(void)now;int state=s->state;
    if(state==PJ_TCS_IDLE)return PJ_POLL_WAITING;
    s->state=PJ_TCS_IDLE;
    return state==PJ_TCS_READY?PJ_POLL_READY:state==PJ_TCS_REFUSED?PJ_POLL_REFUSED:PJ_POLL_FAILED;
}
static const uint8_t *pj_tcs_reply(void *c,size_t *size){pj_tc_source *s=c;*size=s->reply_size;return s->reply;}
static void pj_tcs_close(void *c){((pj_tc_source *)c)->state=PJ_TCS_IDLE;}
/* 1 when the image holds a Home to show. */
static inline int pj_tc_source_open(pj_transport *t,pj_tc_source *s,const pj_tc_image *image) {
    s->image=image;s->state=PJ_TCS_IDLE;s->reply_size=0;
    if(!image||!image->ready||!pj_tc_find(image,PJ_RPC_HOME,0,0,0))return 0;
    *t=(pj_transport){s,"On this Pocket",1,pj_tcs_idle,pj_tcs_begin,pj_tcs_poll,pj_tcs_reply,pj_tcs_close};
    return 1;
}
#endif
