/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * The APF data-slot table, read the way the Pocket reads it when the core is quit.
 *
 * The Pocket writes two words per slot that has a file into the core's datatable BRAM
 * (word 2n = slot id, word 2n+1 = size), and at quit reads the size word back for every
 * nonvolatile slot: that many bytes of the save window become the .sav file (Pocket debug
 * log: "Reading core data slot BRAM table. Slot ID [10] idx 6 / Slot says it is N bytes").
 * The kernel updates the size word when the app closes a slot it wrote (openfpgaOS
 * save.c nvslot_commit_size, entry resolved by the same id scan as below).
 *
 * The app reads the table through the core's DT_QUERY registers, the registers the kernel
 * itself uses for the scan (openfpgaOS hal/regs.h DT_QUERY 0x40000090, DT_QUERY_DATA
 * 0x40000094; a query is stateless: writing the word address starts one BRAM read on the
 * bridge side, bit 31 of DT_QUERY says the answer is in). The kernel only queries inside
 * file system calls the app itself makes, so the app may query whenever it has no file open
 * and no card read in flight. What it shows on the start screen is therefore what the
 * Pocket will see at quit (after a save) and what it loaded (at start): on-device evidence
 * of the exit save without a PC.
 */
#ifndef PJ_SAVE_TABLE_H
#define PJ_SAVE_TABLE_H
#include <stdint.h>
#define PJ_DT_ENTRIES 64u      /* openfpgaOS file.c DATATABLE_MAX_ENTRIES */
typedef int (*pj_dt_read_fn)(void *context,uint32_t word,uint32_t *value); /* 0 ok, -1 no answer */
typedef struct {int entry;uint32_t size;} pj_dt_entry;   /* entry -1: the slot is not in the table */

/* Finds the table entry whose id word names `slot`, lowest entry first (the kernel's
 * datatable_entry_scan_for_slot). 1 found, 0 absent, -1 the query path gave no answer. */
static inline int pj_dt_find(pj_dt_read_fn read,void *context,unsigned slot,pj_dt_entry *out) {
    out->entry=-1;out->size=0;
    for(unsigned e=0;e<PJ_DT_ENTRIES;e++) {
        uint32_t id=0,size=0;
        if(read(context,e*2u,&id)<0)return -1;
        if((id&0xffffu)!=slot)continue;
        if(read(context,e*2u+1u,&size)<0)return -1;
        out->entry=(int)e;out->size=size;return 1;
    }
    return 0;
}

#if !defined(OF_PC)
#define PJ_DT_QUERY ((volatile uint32_t *)0x40000090u)
#define PJ_DT_QUERY_DATA ((volatile uint32_t *)0x40000094u)
/* One table word over DT_QUERY, as the kernel's datatable_read_word32 does it. */
static inline int pj_dt_read_mmio(void *context,uint32_t word,uint32_t *value) {
    (void)context;
    *PJ_DT_QUERY=word;
    for(int i=0;i<1000;i++) {
        uint32_t v=*PJ_DT_QUERY;
        if(v&0x80000000u) {
            uint32_t legacy=v&0x7fffffffu,full=*PJ_DT_QUERY_DATA;
            *value=(full!=0||legacy==0)?full:legacy;
            return 0;
        }
    }
    return -1;
}
#endif
#endif
