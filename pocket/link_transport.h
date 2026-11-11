/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Tethered source: the existing byte-link RPC client behind pj_transport. */
#ifndef PJ_LINK_TRANSPORT_H
#define PJ_LINK_TRANSPORT_H
#include "rpc_client.h"
#include "transport.h"
static int pj_lt_idle(void *c) {
    pj_rpc_client *r=c;return !r->waiting&&!r->link.tx_active;
}
static int pj_lt_begin(void *c,unsigned command,const char *json,uint32_t now) {
    return pj_rpc_begin(c,command,json,now);
}
static int pj_lt_poll(void *c,uint32_t now) {
    pj_rpc_client *r=c;return r->link.available?pj_rpc_poll(r,now):PJ_POLL_WAITING;
}
static const uint8_t *pj_lt_reply(void *c,size_t *size) {
    pj_rpc_client *r=c;*size=r->response_size-PJ_RPC_HEADER;return r->response+PJ_RPC_HEADER;
}
static void pj_lt_close(void *c) {
    pj_rpc_client *r=c;pj_link_close(&r->link);r->waiting=0;
}
static inline int pj_link_transport_open(pj_transport *t,pj_rpc_client *r,volatile uint32_t *regs) {
    if(!pj_rpc_open(r,regs))return 0;
    *t=(pj_transport){r,"Tethered",0,pj_lt_idle,pj_lt_begin,pj_lt_poll,pj_lt_reply,pj_lt_close};
    return 1;
}
#endif
