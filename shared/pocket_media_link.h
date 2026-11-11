/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Nonblocking MMIO/framing binding for the NEW Pocket media peripheral only.
 * Bind in the kernel/privileged driver if the application cannot access MMIO.
 * It is not compatible with the SDK's existing 33-bit of_net driver.
 */
#ifndef PJ_POCKET_MEDIA_LINK_H
#define PJ_POCKET_MEDIA_LINK_H
#include "link_framing.h"
#define PJ_LINK_BASE 0x4d000000u
#define PJ_LINK_SIGNATURE 0x4b4c4a50u
typedef struct {
    volatile uint32_t *regs;
    pj_wire_rx rx;
    pj_wire_tx tx;
    uint8_t available,tx_active;
} pj_pocket_link;
static inline void pj_link_fence(void) {
#ifdef __riscv
    __asm__ volatile("fence iorw, iorw" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}
static inline uint32_t pj_link_reg_read(pj_pocket_link *l,unsigned index) {
    pj_link_fence();uint32_t result=l->regs[index];pj_link_fence();return result;
}
static inline void pj_link_reg_write(pj_pocket_link *l,unsigned index,uint32_t value) {
    pj_link_fence();l->regs[index]=value;pj_link_fence();
}
static inline int pj_link_open(pj_pocket_link *l,volatile uint32_t *regs,
                                uint8_t *rx_buffer,size_t capacity) {
    if(!l||!regs)return 0;
    l->regs=regs;l->available=0;l->tx_active=0;
    /* Never reset/write a legacy core merely because the MMIO base matches. */
    if(pj_link_reg_read(l,31)!=PJ_LINK_SIGNATURE||
            !pj_wire_rx_init(&l->rx,rx_buffer,capacity))return 0;
    l->available=1;
    pj_link_reg_write(l,0,2); /* reset/flush, pins released */
    pj_link_reg_write(l,0,13); /* enabled master, idle polling */
    return 1;
}
/* Raise the clock once a peer says it can follow. Reopening restores 256 kHz. */
static inline void pj_link_set_khz(pj_pocket_link *l,unsigned khz) {
    if(l&&l->available&&khz>=256)pj_link_reg_write(l,3,90000u/(2u*khz));
}
static inline void pj_link_close(pj_pocket_link *l) {
    if(l&&l->available)pj_link_reg_write(l,0,2);
    if(l){l->available=0;l->tx_active=0;}
}
/* Input belongs to caller until tx_active clears or close. No escaped copy. */
static inline int pj_link_send(pj_pocket_link *l,const uint8_t *packet,size_t size) {
    if(!l||!l->available||l->tx_active||!pj_wire_tx_init(&l->tx,packet,size))return 0;
    l->tx_active=1;return 1;
}
static inline void pj_link_pump_tx(pj_pocket_link *l,unsigned byte_budget) {
    if(!l||!l->available)return;
    while(l->tx_active&&byte_budget--&&(pj_link_reg_read(l,0)&16u)) {
        uint8_t byte;
        if(!pj_wire_tx_next(&l->tx,&byte)){l->tx_active=0;break;}
        pj_link_reg_write(l,1,byte);
    }
}
/* 1 = frame in caller's RX buffer, 0 = incomplete, -1 = framing loss,
 * -2 = hardware fault/disconnected. Caller validates inner CRC and handles.
 * Consume returned bytes before polling again. TX/RX work is bounded per tick.
 */
static inline int pj_link_poll(pj_pocket_link *l,unsigned byte_budget,size_t *size) {
    if(!l||!l->available||!size)return -2;
    if(pj_link_reg_read(l,6))return -2;
    while(byte_budget--&&(pj_link_reg_read(l,0)&32u)) {
        uint8_t byte=(uint8_t)pj_link_reg_read(l,2);
        int result=pj_wire_rx_byte(&l->rx,byte,size);
        if(result)return result;
    }
    return 0;
}
#endif
