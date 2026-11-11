/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Nonblocking decoder service. An entire CRC-validated PJAV JPEG payload must
 * remain owned by caller until completion. Frame storage is caller-owned.
 * Time units passed to begin/service are wrapping uint32 microseconds.
 */
#ifndef PJ_POCKET_JPEG_H
#define PJ_POCKET_JPEG_H
#include "pocket_media_link.h"
#define PJ_JPEG_BASE (PJ_LINK_BASE+0x80u)
#define PJ_JPEG_SIGNATURE 0x47454a50u
#define PJ_JPEG_REVISION 4u /* 4: core input fix (stale-byte false end of image); refuse an older core */
#define PJ_JPEG_MAX_PIXELS (256u*144u)
#define PJ_JPEG_EMPTY 0xffffffffu
typedef struct {
    pj_pocket_link io; /* shared fenced register access; link fields unused */
    const uint8_t *encoded;
    size_t length,offset;
    uint16_t *frame;
    unsigned width,height,pixels;
    uint32_t started_us,stall_us;
    uint8_t available,active,last_sent,stalled;
    /* Why the last frame failed, for the diagnostics log: 0 none, 1 decoder fault
     * (status bit 5; reg 6 in fail_faults), 2 no progress for 250 ms since servicing last
 * made progress,
     * 3 pixel outside the frame. fail_pixels is how far it got. */
    uint8_t fail;uint32_t fail_faults,fail_pixels,fail_us;
#ifdef PJ_JPEG_STRICT
    uint8_t seen[PJ_JPEG_MAX_PIXELS/8]; /* PC harness only; costs per-pixel cycles */
#endif
} pj_pocket_jpeg;
static inline int pj_jpeg_open(pj_pocket_jpeg *j,volatile uint32_t *regs) {
    if(!j||!regs)return 0;
    j->available=0;j->active=0;j->io.regs=regs;
    /* Revision 2 added register 7 and 256-wide frames, 3 the progress watchdog; refuse older
     * rather than fault mid-picture. */
    if(pj_link_reg_read(&j->io,31)!=PJ_JPEG_SIGNATURE||
       pj_link_reg_read(&j->io,30)<PJ_JPEG_REVISION)return 0;
    j->available=1;pj_link_reg_write(&j->io,0,0);return 1;
}
static inline void pj_jpeg_abort(pj_pocket_jpeg *j) {
    if(j&&j->available)pj_link_reg_write(&j->io,0,0);
    if(j)j->active=0;
}
static inline int pj_jpeg_begin(pj_pocket_jpeg *j,const uint8_t *encoded,size_t length,
    uint16_t *frame,unsigned width,unsigned height,uint32_t now_us) {
    if(!j||!j->available||j->active||!encoded||!frame||length<4||length>32768||
       encoded[0]!=255||encoded[1]!=216||(width!=128&&width!=160&&width!=256)||
       (height!=96&&height!=128&&height!=144))return 0;
    j->encoded=encoded;j->length=length;j->offset=0;j->frame=frame;
    j->width=width;j->height=height;j->pixels=0;j->started_us=now_us;j->last_sent=0;
    j->stalled=0;j->stall_us=now_us;j->fail=0;
#ifdef PJ_JPEG_STRICT
    for(unsigned i=0;i<width*height/8;i++)j->seen[i]=0;
#endif
    pj_link_reg_write(&j->io,0,0);
    pj_link_reg_write(&j->io,3,width|(height<<16));
    pj_link_reg_write(&j->io,0,1);j->active=1;return 1;
}
static inline int pj_jpeg_failed(pj_pocket_jpeg *j,uint8_t why,uint32_t now_us) {
    j->fail=why;j->fail_pixels=j->pixels;j->fail_us=now_us-j->started_us;
    j->fail_faults=pj_link_reg_read(&j->io,6);
    pj_jpeg_abort(j);return -1;
}
/* 1 complete, 0 pending, -1 fault/timeout; all failures reset/flush the decoder.
 * The timeout is 250 ms since servicing last made progress. A long pause between calls
 * that then finds progress resets it (the CPU being busy elsewhere tripped valid frames on
 * hardware); once a call has found no progress, time until the next such call counts. 
 * Budget counts pixel-register reads, bounding host work rather than wall-time.
 * Register 7 returns all-ones when empty, so a pixel costs one bus read, not a
 * status read plus a pop. Returns early once there is nothing to do; continue
 * servicing audio/link/UI between calls.
 */
static inline int pj_jpeg_service(pj_pocket_jpeg *j,unsigned budget,uint32_t now_us) {
    if(!j||!j->active)return -1;
    if(!budget)return 0; /* not serviced: neither progress nor stall */
    if(budget>8192)budget=8192;
    int progressed=0;
    const unsigned width=j->width,height=j->height;
    volatile uint32_t *pixel_reg=j->io.regs+7;
    uint16_t *frame=j->frame;
    while(budget) {
        uint32_t status=pj_link_reg_read(&j->io,0);
        if(status&32u)return pj_jpeg_failed(j,1,now_us);
        unsigned fed=0,drained=0;
        if(status&2u) {
            if(j->offset<j->length) {
                uint32_t word=0;
                for(unsigned i=0;i<4&&j->offset<j->length;i++)word|=(uint32_t)j->encoded[j->offset++]<<(8*i);
                pj_link_reg_write(&j->io,1,word);fed=1;
            } else if(!j->last_sent) {
                pj_link_reg_write(&j->io,2,16); /* blank final flush record */
                pj_link_reg_write(&j->io,1,0);j->last_sent=1;fed=1;
            }
        }
        while(budget&&drained<64) {
            uint32_t pixel=*pixel_reg;budget--;
            if(pixel==PJ_JPEG_EMPTY)break;
            unsigned x=pixel>>24,y=(pixel>>16)&255;
            if(x>=width||y>=height)return pj_jpeg_failed(j,3,now_us);
            unsigned index=y*width+x;
#ifdef PJ_JPEG_STRICT
            uint8_t mask=(uint8_t)(1u<<(index&7));
            if(j->seen[index>>3]&mask)return pj_jpeg_failed(j,3,now_us);
            j->seen[index>>3]|=mask;
#endif
            frame[index]=(uint16_t)pixel;j->pixels++;drained++;
        }
        if((status&8u)&&j->pixels==width*height&&j->last_sent) {
            pj_jpeg_abort(j);return 1;
        }
        if(fed||drained)progressed=1;
        if(!fed&&!drained)break; /* decoder busy: let audio and input run */
    }
    if(progressed){j->stalled=0;return 0;}
    if(!j->stalled){j->stalled=1;j->stall_us=now_us;return 0;}
    if((uint32_t)(now_us-j->stall_us)>=250000u)return pj_jpeg_failed(j,2,now_us);
    return 0;
}
#endif
