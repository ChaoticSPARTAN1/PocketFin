/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * On-card source: answers the same requests as the PC Gateway from a library the
 * PC recorded onto the SD card, so no network or companion device is needed while
 * watching. Reads are asynchronous; nothing here may stall audio or video.
 *
 * library.pjl (data slot 4), little-endian, written by tools/pocket_library.py:
 *   header  32 bytes: "PJLB", version, record count, record table offset,
 *                     title count, title table offset, file size, table CRC32
 *   record  24 bytes: command u8, kind u8, pad u16, handle, start, offset, size,
 *                     reserved. Sorted by no key; the table is small and in RAM.
 *   title   32 bytes: handle, session, slot, offset, length, config length,
 *                     seek table offset, seek entry count
 * A title is plain concatenated PJAV packets inside a pack file; seek entries are
 * byte offsets from the title start, one per PJ_SD_SEEK_SECONDS.
 */
#ifndef PJ_SD_LIBRARY_H
#define PJ_SD_LIBRARY_H
#include <stdio.h>
#include "../shared/bounded_json.h"
#include "../shared/bridge_rpc.h"
#include "transport.h"
#define PJ_SD_VERSION 1u
#define PJ_SD_LIBRARY_SLOT 4u
#define PJ_SD_HEADER 32u
#define PJ_SD_RECORD 24u
#define PJ_SD_TITLE 32u
#define PJ_SD_MAX_RECORDS 32768u /* must equal MAX_RECORDS in tools/pocket_library.py */
#define PJ_SD_MAX_TITLES 256u
#define PJ_SD_SEEK_SECONDS 5u
#define PJ_SD_CHUNK 32752u /* READ payload is a 16-byte header plus this */
#define PJ_SD_TIMEOUT_US 3000000u

#ifndef OF_PC
#include "of_file.h"
#include "of_error.h"
static volatile int pj_sd_io_done,pj_sd_io_result;
static void pj_sd_io_complete(int token,int result) {
    (void)token;pj_sd_io_result=result;pj_sd_io_done=1;
}
/* 1 submitted, 0 try again, negative failure. */
static int pj_sd_io_start(unsigned slot,uint32_t offset,void *dest,uint32_t size) {
    pj_sd_io_done=0;
    int token=of_file_read_async((int)slot,offset,dest,size,pj_sd_io_complete);
    if(token==OF_ERR_BUSY){of_file_async_poll();return 0;}
    return token<0?token:1;
}
/* 1 complete, 0 pending, negative failure.
 * The completion callback runs from an interrupt, which clears `busy` and sets
 * `done` together. Reading `done` and then `busy` could see neither and call a
 * good read lost, so `done` is read again after `busy`. */
static int pj_sd_io_poll(void) {
    of_file_async_poll();
    if(!pj_sd_io_done) {
        int busy=of_file_async_busy();
        if(!pj_sd_io_done)return busy?0:-2;
    }
    return pj_sd_io_result?-3:1;
}
static uint32_t pj_sd_io_limit(void) {
    uint32_t limit=of_file_async_max_read();return limit?limit:4096;
}
/* 1 when no card read is in flight or waiting to be copied out (the completion
 * callback fires only after the copy), so the save memory can be handed to the CPU. */
static inline int pj_sd_io_quiet(void) {
    of_file_async_poll();
    return !of_file_async_busy();
}
#else
int pj_sd_pc_read(unsigned slot,uint32_t offset,void *dest,uint32_t size);
static int pj_sd_io_result_pc;
static int pj_sd_io_start(unsigned slot,uint32_t offset,void *dest,uint32_t size) {
    pj_sd_io_result_pc=pj_sd_pc_read(slot,offset,dest,size);return 1;
}
static int pj_sd_io_poll(void){return pj_sd_io_result_pc?-1:1;}
static uint32_t pj_sd_io_limit(void){return 32768;}
static inline int pj_sd_io_quiet(void){return 1;}
#endif

typedef enum {PJ_SD_IDLE,PJ_SD_SUBMIT,PJ_SD_READING,PJ_SD_READY,PJ_SD_REFUSED,PJ_SD_FAILED} pj_sd_state;
typedef enum {PJ_SD_THEN_REPLY,PJ_SD_THEN_START} pj_sd_next;
typedef struct {
    uint8_t records[PJ_SD_MAX_RECORDS*PJ_SD_RECORD],titles[PJ_SD_MAX_TITLES*PJ_SD_TITLE];
    uint8_t reply[PJ_RPC_LIMIT+16];
    uint32_t record_count,title_count,file_size;
    size_t reply_size;
    pj_sd_state state;pj_sd_next next;
    unsigned io_slot;uint32_t io_offset,io_left,io_started;uint8_t *io_dest;
    uint32_t io_bytes,io_chunks; /* totals since open, for the diagnostics log */
    /* Open stream */
    const uint8_t *title;uint32_t session,position,start_seconds,seek_value;uint8_t config_pending;
    uint64_t logical;
    const char *why; /* last failure, for the source screen */
} pj_sd_library;

static inline const uint8_t *pj_sd_record(const pj_sd_library *s,unsigned command,unsigned kind,
                                         uint32_t handle,uint32_t start) {
    for(uint32_t i=0;i<s->record_count;i++) {
        const uint8_t *r=s->records+i*PJ_SD_RECORD;
        if(r[0]==command&&r[1]==kind&&pj_av_u32(r+4)==handle&&pj_av_u32(r+8)==start)return r;
    }
    return NULL;
}
static inline const uint8_t *pj_sd_title(const pj_sd_library *s,uint32_t handle) {
    for(uint32_t i=0;i<s->title_count;i++) {
        const uint8_t *t=s->titles+i*PJ_SD_TITLE;
        if(pj_av_u32(t)==handle)return t;
    }
    return NULL;
}
static inline void pj_sd_read(pj_sd_library *s,unsigned slot,uint32_t offset,void *dest,
                             uint32_t size,uint32_t now,pj_sd_next next) {
    s->io_slot=slot;s->io_offset=offset;s->io_dest=dest;s->io_left=size;s->io_started=now;
    s->next=next;s->state=size?PJ_SD_SUBMIT:PJ_SD_READY;
}
/* Advance reads; bounded, never spins. */
static inline void pj_sd_pump(pj_sd_library *s,uint32_t now) {
    if(s->state==PJ_SD_READING) {
        int result=pj_sd_io_poll();
        if(result<0){s->state=PJ_SD_FAILED;s->why=result==-2?"read lost":"card read error";return;}
        if(result) {
            uint32_t limit=pj_sd_io_limit(),chunk=s->io_left<limit?s->io_left:limit;
            s->io_offset+=chunk;s->io_dest+=chunk;s->io_left-=chunk;
            s->state=s->io_left?PJ_SD_SUBMIT:PJ_SD_READY;
            /* The deadline is for a stalled card, not a long read: each finished chunk
             * restarts it. A 600 KB library index is many chunks and may take seconds. */
            s->io_started=now;s->io_bytes+=chunk;s->io_chunks++;
        }
    }
    if(s->state==PJ_SD_SUBMIT) {
        uint32_t limit=pj_sd_io_limit(),chunk=s->io_left<limit?s->io_left:limit;
        int result=pj_sd_io_start(s->io_slot,s->io_offset,s->io_dest,chunk);
        if(result<0){s->state=PJ_SD_FAILED;s->why="read refused";}
        else if(result)s->state=PJ_SD_READING;
    }
    if((s->state==PJ_SD_SUBMIT||s->state==PJ_SD_READING)&&
       (uint32_t)(now-s->io_started)>=PJ_SD_TIMEOUT_US){s->state=PJ_SD_FAILED;s->why="card too slow";}
}
/* Open is the one blocking step: it runs before any playback exists. */
static inline int pj_sd_wait(pj_sd_library *s,uint32_t (*clock_us)(void)) {
    while(s->state==PJ_SD_SUBMIT||s->state==PJ_SD_READING)pj_sd_pump(s,clock_us());
    int ok=s->state==PJ_SD_READY;s->state=PJ_SD_IDLE;return ok;
}
static int pj_sd_idle(void *c) {return ((pj_sd_library *)c)->state==PJ_SD_IDLE;}
static inline void pj_sd_text(pj_sd_library *s,const char *json) {
    s->reply_size=strlen(json);memcpy(s->reply,json,s->reply_size);s->state=PJ_SD_READY;s->next=PJ_SD_THEN_REPLY;
}
static inline int pj_sd_serve(pj_sd_library *s,const uint8_t *record,uint32_t now) {
    uint32_t size=pj_av_u32(record+16);
    if(size>PJ_RPC_LIMIT){s->state=PJ_SD_FAILED;s->why="library damaged";return 1;}
    s->reply_size=size;
    pj_sd_read(s,PJ_SD_LIBRARY_SLOT,pj_av_u32(record+12),s->reply,size,now,PJ_SD_THEN_REPLY);
    return 1;
}
static inline void pj_sd_put_read_header(uint8_t *p,uint32_t session,uint64_t offset,unsigned kind,unsigned size) {
    pj_put32(p,session);pj_put32(p+4,(uint32_t)offset);pj_put32(p+8,(uint32_t)(offset>>32));
    p[12]=(uint8_t)kind;p[13]=0;p[14]=(uint8_t)size;p[15]=(uint8_t)(size>>8);
}
static int pj_sd_begin(void *c,unsigned command,const char *json,uint32_t now) {
    pj_sd_library *s=c;
    if(s->state!=PJ_SD_IDLE)return 0;
    pj_json_token tokens[32];pj_json j;uint64_t handle=0,value=0;
    if(!pj_json_parse(&j,(const uint8_t *)json,strlen(json),tokens,32))return 0;
    int at=pj_json_find(&j,0,"handle");
    if(at>=0&&!pj_json_u64(&j,at,&handle))return 0;
    const uint8_t *record;
    switch(command) {
    case PJ_RPC_HELLO:
        pj_sd_text(s,"{\"version\":1,\"transport\":\"sd-library\",\"compressed_stream\":\"PJAV-JPEG-IMA\"}");
        return 1;
    case PJ_RPC_HOME:
        record=pj_sd_record(s,command,0,0,0);break;
    case PJ_RPC_BROWSE:
        at=pj_json_find(&j,0,"start");
        if(at>=0&&!pj_json_u64(&j,at,&value))return 0;
        record=pj_sd_record(s,command,0,(uint32_t)handle,(uint32_t)value);break;
    case PJ_RPC_DETAILS:
        record=pj_sd_record(s,command,0,(uint32_t)handle,0);break;
    case PJ_RPC_ART: {
        uint64_t kind=0,offset=0;
        at=pj_json_find(&j,0,"kind");
        if(at>=0&&!pj_json_u64(&j,at,&kind))return 0;
        at=pj_json_find(&j,0,"offset");
        if(at>=0&&!pj_json_u64(&j,at,&offset))return 0;
        if(kind>PJ_ART_BACKDROP)return 0;
        record=pj_sd_record(s,command,(unsigned)kind,(uint32_t)handle,0);
        uint32_t total=record?pj_av_u32(record+16):0;
        if(offset>total)return 0;
        uint32_t size=total-(uint32_t)offset;
        if(size>PJ_RPC_LIMIT-8)size=PJ_RPC_LIMIT-8;
        pj_put32(s->reply,total);
        s->reply[4]=(uint8_t)pj_art_width((unsigned)kind);s->reply[5]=(uint8_t)(pj_art_width((unsigned)kind)>>8);
        s->reply[6]=(uint8_t)pj_art_height((unsigned)kind);s->reply[7]=(uint8_t)(pj_art_height((unsigned)kind)>>8);
        s->reply_size=8+size;
        if(!record){s->state=PJ_SD_READY;s->next=PJ_SD_THEN_REPLY;return 1;} /* art may be absent */
        pj_sd_read(s,PJ_SD_LIBRARY_SLOT,pj_av_u32(record+12)+(uint32_t)offset,s->reply+8,size,now,PJ_SD_THEN_REPLY);
        return 1;
    }
    case PJ_RPC_START: {
        const uint8_t *title=pj_sd_title(s,(uint32_t)handle);
        if(!title){s->state=PJ_SD_REFUSED;return 1;}
        at=pj_json_find(&j,0,"start_seconds");
        if(at>=0&&!pj_json_u64(&j,at,&value))return 0;
        uint32_t entry=(uint32_t)(value/PJ_SD_SEEK_SECONDS),count=pj_av_u32(title+28);
        if(!count){s->state=PJ_SD_FAILED;s->why="no seek table";return 1;}
        if(entry>=count)entry=count-1;
        s->title=title;s->session=pj_av_u32(title+4);s->start_seconds=entry*PJ_SD_SEEK_SECONDS;
        s->config_pending=1;s->logical=0;
        pj_sd_read(s,PJ_SD_LIBRARY_SLOT,pj_av_u32(title+24)+entry*4u,&s->seek_value,4,now,PJ_SD_THEN_START);
        return 1;
    }
    case PJ_RPC_READ: {
        uint64_t session=0,offset=0;
        if(!pj_json_u64(&j,pj_json_find(&j,0,"session"),&session)||
           !pj_json_u64(&j,pj_json_find(&j,0,"offset"),&offset))return 0;
        if(!s->title||session!=s->session||offset!=s->logical){s->state=PJ_SD_FAILED;s->why="read out of step";return 1;}
        uint32_t length=pj_av_u32(s->title+16),base=pj_av_u32(s->title+12),slot=pj_av_u32(s->title+8);
        uint32_t from,size;
        if(s->config_pending){from=0;size=pj_av_u32(s->title+20);}
        else {
            from=s->position;size=length-s->position;
            if(size>PJ_SD_CHUNK)size=PJ_SD_CHUNK;
        }
        if(!size) {
            pj_sd_put_read_header(s->reply,s->session,offset,3,0);
            s->reply_size=16;s->state=PJ_SD_READY;s->next=PJ_SD_THEN_REPLY;return 1;
        }
        pj_sd_put_read_header(s->reply,s->session,offset,1,size);
        s->reply_size=16u+size;s->logical+=size;
        if(s->config_pending)s->config_pending=0;
        else s->position+=size;
        pj_sd_read(s,slot,base+from,s->reply+16,size,now,PJ_SD_THEN_REPLY);
        return 1;
    }
    case PJ_RPC_STOP:
        s->title=NULL;pj_sd_text(s,"{\"stopped\":true}");return 1;
    default:return 0;
    }
    if(!record){s->state=PJ_SD_REFUSED;return 1;}
    return pj_sd_serve(s,record,now);
}
static int pj_sd_poll(void *c,uint32_t now) {
    pj_sd_library *s=c;
    pj_sd_pump(s,now);
    if(s->state==PJ_SD_FAILED){s->state=PJ_SD_IDLE;return PJ_POLL_FAILED;}
    if(s->state==PJ_SD_REFUSED){s->state=PJ_SD_IDLE;return PJ_POLL_REFUSED;}
    if(s->state!=PJ_SD_READY)return PJ_POLL_WAITING;
    if(s->next==PJ_SD_THEN_START) {
        uint32_t seek=s->seek_value; /* card is little-endian, like the CPU */
        if(seek>=pj_av_u32(s->title+16)){s->state=PJ_SD_IDLE;s->why="bad seek point";return PJ_POLL_FAILED;}
        s->position=seek;
        s->reply_size=(size_t)snprintf((char *)s->reply,64,"{\"session\":%lu,\"start_seconds\":%lu}",
            (unsigned long)s->session,(unsigned long)s->start_seconds);
    }
    s->state=PJ_SD_IDLE;return PJ_POLL_READY;
}
static const uint8_t *pj_sd_reply(void *c,size_t *size) {
    pj_sd_library *s=c;*size=s->reply_size;return s->reply;
}
static void pj_sd_close(void *c) {
    pj_sd_library *s=c;
    /* A submitted read still owns its buffer; let it land before reuse. */
    s->title=NULL;
    if(s->state==PJ_SD_READING)while(!pj_sd_io_poll()){}
    s->state=PJ_SD_IDLE;
}
/* 1 usable library, 0 absent or damaged. Fills `titles` for the diagnostics line. */
static inline int pj_sd_open(pj_transport *t,pj_sd_library *s,uint32_t (*clock_us)(void)) {
    uint8_t header[PJ_SD_HEADER];
    s->state=PJ_SD_IDLE;s->title=NULL;s->record_count=s->title_count=0;s->why="";
    s->io_bytes=s->io_chunks=0;
    pj_sd_read(s,PJ_SD_LIBRARY_SLOT,0,header,PJ_SD_HEADER,clock_us(),PJ_SD_THEN_REPLY);
    /* Each failure leaves its reason in `why`, so the screen never just says "none". */
    if(!pj_sd_wait(s,clock_us)) {
        /* An empty or missing slot is refused at the first read. */
        if(!*s->why||!strcmp(s->why,"read refused"))s->why="no library file; run PC sync";
        return 0;
    }
    if(memcmp(header,"PJLB",4)){s->why="not a library file";return 0;}
    if(pj_av_u32(header+4)!=PJ_SD_VERSION){s->why="library version differs; run PC sync";return 0;}
    uint32_t records=pj_av_u32(header+8),record_at=pj_av_u32(header+12);
    uint32_t titles=pj_av_u32(header+16),title_at=pj_av_u32(header+20);
    if(records>PJ_SD_MAX_RECORDS||titles>PJ_SD_MAX_TITLES){s->why="library too large for this app";return 0;}
    pj_sd_read(s,PJ_SD_LIBRARY_SLOT,record_at,s->records,records*PJ_SD_RECORD,clock_us(),PJ_SD_THEN_REPLY);
    if(!pj_sd_wait(s,clock_us))return 0;
    pj_sd_read(s,PJ_SD_LIBRARY_SLOT,title_at,s->titles,titles*PJ_SD_TITLE,clock_us(),PJ_SD_THEN_REPLY);
    if(!pj_sd_wait(s,clock_us))return 0;
    uint32_t crc=pj_av_crc_part(0xffffffffu,s->records,records*PJ_SD_RECORD);
    crc=pj_av_crc_part(crc,s->titles,titles*PJ_SD_TITLE)^0xffffffffu;
    if(crc!=pj_av_u32(header+28)){s->why="library damaged; run PC sync";return 0;}
    s->record_count=records;s->title_count=titles;s->file_size=pj_av_u32(header+24);
    *t=(pj_transport){s,"On this Pocket",1,pj_sd_idle,pj_sd_begin,pj_sd_poll,pj_sd_reply,pj_sd_close};
    return 1;
}
#endif
