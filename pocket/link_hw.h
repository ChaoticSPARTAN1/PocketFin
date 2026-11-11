/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * The Pocket's link block beyond the shared driver (shared/pocket_media_link.h): clock rates
 * from 8 kHz up, a clean re-sync for the peer, and the session's link numbers for the screen
 * and the log. The wire framing and the shared driver are unchanged.
 *
 * Registers used here (rtl/pocket_media_link.v):
 *   0  control/status   write: enable 1, soft reset 2, master 4, idle polling 8
 *                       read:  busy 64, RX byte waiting 32, TX room 16
 *   3  half period in 90 MHz cycles (45000 / kHz). A core without the slow rates clamps a
 *      slow request to 175 (256 kHz) and reads that back.
 *   4  bytes waiting to be sent
 *   6  faults, write 1 to clear: timeout 1, receive overrun 2, send drop 4, empty read 8
 *   7  slowest half period this core can run (5625 = 8 kHz); 0 on a core from before
 *      2026-11-09, which runs 256 kHz and faster only
 *
 * Re-sync. The Chromatic's receiver finds the byte boundary only after the clock has been
 * high and still for 1 ms. With control = 5 (enable + master, polling off) and nothing left to
 * send, the Pocket drives the clock high and still; PJ_LINK_RESYNC_US of that, then polling
 * on again. Proven against the Chromatic bridge RTL in a test bench in the Jellymatic
 * repository (https://github.com/ChaoticSPARTAN1/Jellymatic).
 */
#ifndef PJ_LINK_HW_H
#define PJ_LINK_HW_H
#include <stddef.h>
#include <stdint.h>
#define PJ_LINK_SAFE_KHZ 256u    /* the reset rate; every core runs it */
#define PJ_LINK_PLAY_KHZ 256u    /* slower than this carries the library and pictures, not video */
#define PJ_LINK_TOP_KHZ 512u     /* never asked faster: 1 MHz does not pass the Chromatic's input filter */
/* 2026-11-10 Core Settings "Link speed" (pocket/core_settings.h): the three figures below (ACKs, the cap on the climb,
 * the filler period below 512 kHz) are chosen at run time, pj_link_cfg, from the menu:
 *   Auto  ACKs on, at most 384 kHz, filler 8 ms: the ack384 test build tried on a console (~25 KB/s,
 *         0 damaged frames); the release default. A rate that damages frames still steps down to 256 (schedule()).
 *   Safe  ACKs on, at most 256 kHz, filler 10 ms.
 *   Fast  no ACKs (the lone 7E filler, "no speed reports"), up to 512 kHz, filler 8 ms there and 10 ms at 256: the
 *         release before 2026-11-10.
 * A build that defines any of PJ_LINK_ACKS / PJ_LINK_MAX_KHZ / PJ_LINK_FILL_SLOW_US is a diagnostic build: its figures
 * stand and the menu's Link speed is not applied (PJ_LINK_BUILD_FIXED). The figures of a build that defines none of
 * them are the Fast ones until the settings are applied at start (live_player.c startup). */
#if defined(PJ_LINK_ACKS)||defined(PJ_LINK_MAX_KHZ)||defined(PJ_LINK_FILL_SLOW_US)
#define PJ_LINK_BUILD_FIXED 1
#else
#define PJ_LINK_BUILD_FIXED 0
#endif
/* A diagnostic build's cap on the climb: the fastest rate taken from the Chromatic's HELLO "link_khz" (every climb is
 * bounded by that). 2026-11-10 ACK test build: -DPJ_LINK_ACKS=1 -DPJ_LINK_MAX_KHZ=256u keeps the link at 256 kHz with
 * ACKs on, to tell a 512 kHz timing-margin fault from crosstalk. Default: the top rate. */
#ifndef PJ_LINK_MAX_KHZ
#define PJ_LINK_MAX_KHZ PJ_LINK_TOP_KHZ
#endif
#define PJ_LINK_RESYNC_US 4000u  /* clock high and still before a HELLO (the peer needs 1 ms) */
/* The filler. The Chromatic's bridge tells its MCU how full its queue toward the Pocket is only
 * in a status packet, and the MCU sends more only against that figure. With nothing coming from
 * the Pocket a status goes out every 50 ms, which holds a long answer (a READ) to 18.8 KB/s end
 * to end whatever the cable carries. The bridge also sends a status as soon as a byte from the
 * Pocket waits (idle FF bytes are not such bytes). So while the Pocket only waits for an answer
 * it sends one frame delimiter (7E) every pj_link_fill_us(): between frames the Chromatic's
 * decoder takes it for nothing (shared/link_framing.h: an empty frame), and every one brings a
 * fresh queue level. Two periods, measured in Jellymatic's tether simulation with the launcher's gateway
 * code (shared/pocket_tether.h has the figures the stream profiles are sized from):
 *   512 kHz: 8 ms. 41.0 KB/s; the queue never passes 64 bytes (the cable empties it faster than
 *            the MCU's UART fills it), so the launcher's own 10 ms pass is the limit.
 *   256 kHz: 10 ms. 29.3 KB/s of the cable's 29.9, the queue at most 466 of 1,024 bytes. This
 *            period goes with the launcher's credit cure (at a status it also counts what went
 *            onto its UART in the pass before; decided 2026-11-10 for every rate). Without that
 *            cure a short period lets the queue reach 751, and the period was 28 ms (26.7 KB/s,
 *            the figure shared/pocket_tether.h still sizes the 256 kHz profiles from).
 * Not below PJ_LINK_FILL_KHZ: a slower cable empties the queue slower still, and there the
 * 50 ms rule is what keeps it from overflowing. Never inside a frame, never while the clock is
 * held still, never for HELLO (rpc_client.h). */
#ifndef PJ_LINK_FILL_FAST_US
#define PJ_LINK_FILL_FAST_US 8000u
#endif
#ifndef PJ_LINK_FILL_SLOW_US
#define PJ_LINK_FILL_SLOW_US 10000u
#endif
#ifndef PJ_LINK_FILL_KHZ
#define PJ_LINK_FILL_KHZ 256u
#endif
typedef struct {
    uint32_t sent,received,good,bad,fault_events; /* bytes out/in, frames good/bad, fault reads */
    unsigned khz,slowest_khz,faults;              /* rate in use; slowest this core runs; fault bits seen */
    uint8_t hold;                                 /* 1: nothing is sent (the clock is being held still) */
    /* 2026-11-10 (launcher 1.5.0, shared/pocket_tether.h "Pocket-acknowledged credit"): ack_on, the Chromatic's
     * HELLO asked for ACKs; ack_ok, an anchor is held: the last whole valid reply's sequence byte (ack_seq) and
     * `received` at its closing 7E (ack_rx); ack_faults, fault_events when it was taken; acks, ACK frames sent. */
    uint8_t ack_on,ack_ok,ack_seq;
    uint32_t ack_rx,ack_faults,acks;
} pj_link_numbers;
/* 2026-11-10 field result: OFF. Launcher 1.5.0 + this app sending ACKs (app 51e7df9a) broke the link on the console
 * (upstream requests arrived damaged and merged into 700-byte frames with no 7E in them, link-byte timeouts every few
 * seconds, the app kept falling back to its menu); the same cable ran 21 minutes clean with the lone 7E (5d972a8a).
 * Not a bridge capacity limit: its upstream path carries 6 bytes a status at least every 66 UART byte times (about
 * 4.7 KB/s at 524,288 baud) against the ACKs' 875 B/s, and the RTL-faithful model peaks its 1 KiB receive queue at
 * 74 bytes. A frame of hundreds of bytes without a 7E cannot come from an intact ACK stream (a 7E every 7 bytes), and
 * a link-byte timeout means the bridge's bit count was out of step when the clock paused: the bytes were damaged on
 * the cable, which the ACKs' 7x more upstream traffic during replies exposes. So the app sends the lone 7E again (the
 * wire behaviour proven on the console) and launcher 1.5.0 uses its 1.4.0 credit rule, as with any app without ACKs
 * (Jellymatic's Pocket link docs). 1 = the ACK path (host checks and a supervised trial only). */
#ifndef PJ_LINK_ACKS
#define PJ_LINK_ACKS 0
#endif
/* A host check's negative control only: an ACK that claims this many bytes more than arrived. */
#ifndef PJ_LINK_ACK_LIE
#define PJ_LINK_ACK_LIE 0u
#endif
/* The link speed in use (see "Core Settings" above). */
typedef struct {unsigned acks,max_khz,fill_slow_us;} pj_link_mode;
static pj_link_mode pj_link_cfg={PJ_LINK_ACKS,PJ_LINK_MAX_KHZ,PJ_LINK_FILL_SLOW_US};
enum {PJ_LINK_MODE_AUTO=1,PJ_LINK_MODE_SAFE,PJ_LINK_MODE_FAST}; /* = core_settings.h PJ_LINKSET_* */
static inline pj_link_mode pj_link_mode_for(unsigned mode) {
    pj_link_mode auto384={1u,384u,8000u},safe={1u,PJ_LINK_SAFE_KHZ,10000u},fast={0u,PJ_LINK_TOP_KHZ,10000u};
    return mode==PJ_LINK_MODE_SAFE?safe:mode==PJ_LINK_MODE_FAST?fast:auto384;
}
/* One link per app; kept outside the RPC client so a reopen does not zero the counts. */
static pj_link_numbers pj_ln={0,0,0,0,0,PJ_LINK_SAFE_KHZ,PJ_LINK_SAFE_KHZ,0,0,0,0,0,0,0,0};
/* Time for `bytes` at the rate in use, with a quarter to spare (8 bits each, 10000/kHz us). */
static inline uint32_t pj_link_bytes_us(uint32_t bytes) {
    unsigned khz=pj_ln.khz?pj_ln.khz:PJ_LINK_SAFE_KHZ;
    return (uint32_t)((uint64_t)bytes*10000u/khz);
}
/* Raw bytes per second each way at a rate (the Chromatic's own queue and UART are separate). */
static inline unsigned pj_link_raw_bps(unsigned khz){return khz*125u;}
static inline int pj_link_can_play(void){return pj_ln.khz>=PJ_LINK_PLAY_KHZ;}
static inline uint32_t pj_link_fill_us(void){return pj_ln.khz>=PJ_LINK_TOP_KHZ?PJ_LINK_FILL_FAST_US:pj_link_cfg.fill_slow_us;}
/* Which stream profile a rate carries: pj_tether_profile_for() in shared/pocket_tether.h, from
 * the measured end-to-end figures there (the cable rates above are not what a stream gets). */
/* "none", or the names of the fault bits seen: "overrun", "timeout+overrun"... */
static inline const char *pj_link_fault_words(unsigned bits,char *out,size_t size) {
    static const char *const names[]={"timeout","overrun","send drop","empty read"};
    size_t used=0;
    if(!size)return out;
    out[0]=0;
    for(unsigned i=0;i<4;i++)if(bits&(1u<<i)) {
        const char *name=names[i];
        if(used&&used+1<size)out[used++]='+';
        while(*name&&used+1<size)out[used++]=*name++;
        out[used]=0;
    }
    if(!used){const char *none="none";while(*none&&used+1<size)out[used++]=*none++;out[used]=0;}
    return out;
}
#ifndef PJ_LINK_HW_HOST
/* Slowest rate in kHz: 8 on the 2026-11-09 core, 256 on an older one. */
static inline unsigned pj_hw_slowest_khz(pj_pocket_link *l) {
    uint32_t half=pj_link_reg_read(l,7);
    return half>=175u&&half<=45000u?45000u/half:PJ_LINK_SAFE_KHZ;
}
/* Ask for a rate; returns the rate the core took (256 when it cannot go slower). */
static inline unsigned pj_hw_rate(pj_pocket_link *l,unsigned khz) {
    if(!khz)khz=PJ_LINK_SAFE_KHZ;
    uint32_t half=45000u/khz;if(!half)half=1;
    pj_link_reg_write(l,3,half);
    uint32_t back=pj_link_reg_read(l,3);
    if(back==half)return khz;
    if(!back||back==175u)return PJ_LINK_SAFE_KHZ;
    return (45000u+back/2u)/back;
}
/* Faults are read and cleared, never left to stop the link: a damaged frame is asked again. */
static inline unsigned pj_hw_take_faults(pj_pocket_link *l) {
    uint32_t bits=pj_link_reg_read(l,6)&15u;
    if(bits){pj_link_reg_write(l,6,bits);pj_ln.faults|=bits;pj_ln.fault_events++;}
    return bits;
}
/* Soft reset: both queues emptied, 256 kHz again, lines released; a half-received frame is
 * forgotten with them. */
static inline void pj_hw_reset(pj_pocket_link *l) {
    pj_hw_take_faults(l);pj_link_reg_write(l,0,2);
    l->rx.length=0;l->rx.active=0;l->rx.escaped=0;
    pj_ln.ack_ok=0; /* the receive queue's bytes are gone uncounted: no anchor until the next whole reply */
}
static inline void pj_hw_mode(pj_pocket_link *l,int polling){pj_link_reg_write(l,0,polling?13u:5u);}
/* Nothing on the wire and nothing waiting: the clock is high and still. */
static inline int pj_hw_quiet(pj_pocket_link *l) {
    return !(pj_link_reg_read(l,0)&64u)&&!pj_link_reg_read(l,4);
}
static inline void pj_hw_pump(pj_pocket_link *l,unsigned budget) {
    if(pj_ln.hold)return;
    while(l->tx_active&&budget--&&(pj_link_reg_read(l,0)&16u)) {
        uint8_t byte;
        if(!pj_wire_tx_next(&l->tx,&byte)){l->tx_active=0;break;}
        pj_link_reg_write(l,1,byte);pj_ln.sent++;
    }
}
/* One filler byte behind whatever is already waiting (the send queue keeps the order, so it can
 * not land inside a frame: the caller sends it only with no frame being handed over). */
static inline int pj_hw_fill(pj_pocket_link *l) {
    if(pj_ln.hold||l->tx_active||!(pj_link_reg_read(l,0)&16u))return 0;
    pj_link_reg_write(l,1,0x7eu);pj_ln.sent++;return 1;
}
/* The ACK frame (already wire-encoded, at most 14 bytes) behind whatever waits, under the filler's rules: whole
 * or not at all, so it never lands inside a request (pj_link_send starts only behind it, in order). */
static inline int pj_hw_ack(pj_pocket_link *l,const uint8_t *wire,unsigned n) {
    if(pj_ln.hold||l->tx_active||pj_link_reg_read(l,4)+n>1000u)return 0;
    for(unsigned i=0;i<n;i++){pj_link_reg_write(l,1,wire[i]);pj_ln.sent++;}
    return 1;
}
/* Bytes in the receive queue (register 5): taken off the cable, not yet read. */
static inline uint32_t pj_hw_rx_waiting(pj_pocket_link *l) {
    uint32_t n=pj_link_reg_read(l,5);return n<=1024u?n:0u;
}
/* 1 frame, 0 nothing whole yet, -1 a frame was dropped, -2 the link is closed. */
static inline int pj_hw_poll(pj_pocket_link *l,unsigned budget,size_t *size) {
    if(!l->available)return -2;
    pj_hw_take_faults(l);
    while(budget--&&(pj_link_reg_read(l,0)&32u)) {
        uint8_t byte=(uint8_t)pj_link_reg_read(l,2);pj_ln.received++;
        int result=pj_wire_rx_byte(&l->rx,byte,size);
        if(result)return result;
    }
    return 0;
}
#else
/* PC checks model the link: they supply these eight next to their pj_link_* model, and count
 * the bytes themselves. */
static int pj_hw_fill(pj_pocket_link *l);
static int pj_hw_ack(pj_pocket_link *l,const uint8_t *wire,unsigned n); /* 2026-11-10: the eighth hook */
static inline uint32_t pj_hw_rx_waiting(pj_pocket_link *l){(void)l;return 0;} /* the models deliver every byte at once */
static unsigned pj_hw_slowest_khz(pj_pocket_link *l);
static unsigned pj_hw_rate(pj_pocket_link *l,unsigned khz);
static unsigned pj_hw_take_faults(pj_pocket_link *l);
static void pj_hw_reset(pj_pocket_link *l);
static void pj_hw_mode(pj_pocket_link *l,int polling);
static int pj_hw_quiet(pj_pocket_link *l);
static inline void pj_hw_pump(pj_pocket_link *l,unsigned budget){if(!pj_ln.hold)pj_link_pump_tx(l,budget);}
static inline int pj_hw_poll(pj_pocket_link *l,unsigned budget,size_t *size) {
    if(!l->available)return -2;
    pj_hw_take_faults(l);
    int result=pj_link_poll(l,budget,size);
    return result==-2?0:result;
}
#endif
#endif
