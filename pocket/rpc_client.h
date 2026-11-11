/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md */
#ifndef PJ_RPC_CLIENT_H
#define PJ_RPC_CLIENT_H
#include "../shared/bridge_rpc.h"
#include "../shared/pocket_media_link.h"
#include "../shared/pocket_tether.h"
#include "../shared/link_framing.h"
#include "link_hw.h"
typedef struct {
    pj_pocket_link link;uint8_t response[PJ_RPC_HEADER+PJ_RPC_LIMIT],request[1040];
    uint32_t sequence,began_us,last_tx_us,progress_us,seen_rx;size_t request_size,response_size;
    unsigned command,retries;uint8_t waiting;
    uint8_t resync;uint32_t resync_us,fill_us; /* resync 1: waiting for the wire to empty; 2: clock held still */
} pj_rpc_client;
/* A request asked again after silence goes out behind a still clock (PJ_LINK_RESYNC_US), like
 * the first HELLO: silence can mean the Chromatic lost the byte boundary (cable moved, its
 * UART rate change swallowed bytes), and then only a still clock lets it find the next request. */
static inline void pj_rpc_sync_end(pj_rpc_client *c){if(c->resync){pj_hw_mode(&c->link,1);c->resync=0;}}
static inline int pj_rpc_open(pj_rpc_client *c,volatile uint32_t *regs) {
    memset(c,0,sizeof(*c));
    c->seen_rx=pj_ln.received;pj_ln.ack_on=pj_ln.ack_ok=0; /* a new link: no ACKs until its HELLO asks */
    return pj_link_open(&c->link,regs,c->response,sizeof(c->response));
}
static inline int pj_rpc_begin(pj_rpc_client *c,unsigned command,const char *body,uint32_t now) {
    if(c->waiting||c->link.tx_active)return 0;
    pj_rpc_sync_end(c);
    if(++c->sequence==0)c->sequence=1;
    c->request_size=pj_rpc_encode(c->request,sizeof(c->request),command,c->sequence,body,strlen(body));
    if(!c->request_size||!pj_link_send(&c->link,c->request,c->request_size))return 0;
    c->command=command;c->waiting=1;c->began_us=c->last_tx_us=c->progress_us=now;c->retries=0;
    c->seen_rx=pj_ln.received;c->fill_us=now;return 1;
}
/* Give up waiting for the answer (a rate probe that failed); a late answer is then ignored. */
static inline void pj_rpc_cancel(pj_rpc_client *c){c->waiting=0;c->link.tx_active=0;pj_rpc_sync_end(c);}
/* The waits at the link rate in use. At 256 kHz and faster they are the fixed 3 s (ask again)
 * and 15 s (give up). A slower clock adds the time the request itself takes on the wire, and
 * the 15 s then counts from the last byte that arrived, so a long answer at 8 kHz
 * (1,000 bytes a second) is not cut off; one whole largest answer is still the outer bound. */
static inline uint32_t pj_rpc_retry_us(const pj_rpc_client *c) {
    return 3000000u+(pj_ln.khz<PJ_LINK_SAFE_KHZ?pj_link_bytes_us((uint32_t)c->request_size*2u+16u):0u);
}
static inline int pj_rpc_expired(const pj_rpc_client *c,uint32_t now) {
    if(pj_ln.khz>=PJ_LINK_SAFE_KHZ)return (uint32_t)(now-c->began_us)>15000000u;
    uint32_t request=pj_link_bytes_us((uint32_t)c->request_size*2u+16u);
    if((uint32_t)(now-c->progress_us)>15000000u+request)return 1;
    return (uint32_t)(now-c->began_us)>15000000u+request+pj_link_bytes_us((uint32_t)sizeof(c->response));
}
static inline int pj_rpc_resend(pj_rpc_client *c,uint32_t now) {
    if(!pj_link_send(&c->link,c->request,c->request_size))return 0;
    c->last_tx_us=now;c->retries++;return 1;
}
/* 1 response ready, 0 waiting, -1 timeout/device failure, -2 RPC error.
 * Response stays owned until next pump; application consumes it before poll.
 * A damaged frame (bad framing or checksum) is counted and the request is asked again at once,
 * up to the same three times; only after that does a damaged frame fail the request.
 * Silence (a request or an answer lost whole) is not a damaged frame and is not counted as one:
 * the request is asked again after the wait, behind a still clock.
 * While only waiting, a filler byte goes out every pj_link_fill_us() (link_hw.h says why); not for
 * HELLO, which is also what tries a rate the cable may not carry. */
/* 2026-11-10 (launcher 1.5.0): the filler, as an ACK when the Chromatic asked for them and an anchor is held
 * (shared/pocket_tether.h "Pocket-acknowledged credit"), else the lone 7E as before. */
static inline int pj_rpc_fill(pj_rpc_client *c) {
#if defined(PJ_LINK_NO_ACK) /* a host check's "Pocket app from before 1.5.0" */
    if(1)return pj_hw_fill(&c->link);
#endif
    /* Core Settings Link speed (link_hw.h pj_link_cfg): Fast sends none, Auto and Safe send them where asked for */
    if(!pj_link_cfg.acks||!pj_ln.ack_on||!pj_ln.ack_ok)return pj_hw_fill(&c->link);
    uint8_t ack[PJ_TETHER_ACK_SIZE],wire[2u+2u*PJ_TETHER_ACK_SIZE];unsigned n=0;
    pj_tether_ack_build(ack,pj_ln.ack_seq,pj_ln.received-pj_ln.ack_rx+pj_hw_rx_waiting(&c->link)+PJ_LINK_ACK_LIE);
    pj_wire_tx t;if(!pj_wire_tx_init(&t,ack,sizeof(ack)))return 0;
    while(n<sizeof(wire)&&pj_wire_tx_next(&t,&wire[n]))n++;
    if(!pj_hw_ack(&c->link,wire,n))return 0;
    pj_ln.acks++;return 1;
}
static inline int pj_rpc_poll(pj_rpc_client *c,uint32_t now) {
    pj_hw_pump(&c->link,256);size_t size=0;
    int got=pj_hw_poll(&c->link,512,&size);
    if(got==-2)return -1;
    /* the ACK anchor: a fault read or a damaged frame drops it, a whole valid frame (any reply) is the new one */
    if(pj_ln.fault_events!=pj_ln.ack_faults){pj_ln.ack_faults=pj_ln.fault_events;pj_ln.ack_ok=0;}
    if(got>0&&pj_rpc_validate(c->response,size)){pj_ln.ack_ok=1;pj_ln.ack_seq=(uint8_t)pj_av_u32(c->response+8);pj_ln.ack_rx=pj_ln.received;}
    else if(got)pj_ln.ack_ok=0;
    if(pj_ln.received!=c->seen_rx) { /* bytes are arriving: do not ask again over an answer in flight */
        c->seen_rx=pj_ln.received;c->progress_us=now;
        if(c->waiting)c->last_tx_us=now;
    }
    if(got>0&&!pj_rpc_validate(c->response,size))got=-1;
    if(got<0) {
        pj_ln.bad++;
        if(!c->waiting)return 0;
        if(c->retries>=3)return -1;
        if(!c->resync&&!c->link.tx_active&&!pj_ln.hold&&!pj_rpc_resend(c,now))return -1;
        return 0;
    }
    if(got) {
        pj_ln.good++;
        if(c->waiting&&pj_av_u32(c->response+8)==c->sequence&&c->response[4]==c->command) {
            c->waiting=0;c->response_size=size;pj_rpc_sync_end(c);return c->response[5]==3?-2:1;
        }
    }
    if(c->waiting) {
        if(pj_rpc_expired(c,now)){pj_rpc_sync_end(c);return -1;}
        if(c->resync) {
            if(c->resync==1){if(pj_hw_quiet(&c->link)){c->resync=2;c->resync_us=now;}}
            else if((uint32_t)(now-c->resync_us)>=PJ_LINK_RESYNC_US) {
                pj_rpc_sync_end(c);
                if(!pj_rpc_resend(c,now))return -1;
                c->fill_us=now;
            }
        }
        else if(!c->link.tx_active&&!pj_ln.hold&&c->retries<3&&(uint32_t)(now-c->last_tx_us)>pj_rpc_retry_us(c)) {
            pj_hw_mode(&c->link,0);c->resync=1;
        }
        else if(c->command!=PJ_RPC_HELLO&&pj_ln.khz>=PJ_LINK_FILL_KHZ&&!c->link.tx_active&&!pj_ln.hold&&
                (uint32_t)(now-c->fill_us)>=pj_link_fill_us()&&pj_rpc_fill(c))c->fill_us=now;
    }
    return 0;
}
#endif
