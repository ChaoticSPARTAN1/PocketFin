/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Device-path regression for the on-card source: builds sd_library.h WITHOUT
 * OF_PC against a model of the runtime's async read, whose completion callback
 * runs from an interrupt and may land between any two app instructions.
 * The model completes reads at the worst moment: inside of_file_async_busy(),
 * after the app has already looked at its done flag. The first device build
 * treated that as a lost read and stopped playback after a few seconds.
 * Build with -Ipocket/check_sd_async_include so the model headers win.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static uint8_t card[1u<<20];
static int active,completes_in_busy,polls;
static void (*pending_cb)(int,int);
static uint32_t pending_offset,pending_size;static uint8_t *pending_dest;
static void complete(void) {
    memcpy(pending_dest,card+pending_offset,pending_size);
    active=0;pending_cb(1,0); /* interrupt: clear busy and report done together */
}
int of_file_read_async(int slot,uint32_t offset,void *dest,uint32_t size,void (*cb)(int,int)) {
    (void)slot;if(active)return -16;
    if(offset+size>sizeof(card))return -1;
    active=1;pending_cb=cb;pending_offset=offset;pending_size=size;pending_dest=dest;return 1;
}
int of_file_async_poll(void){polls++;return 0;}
int of_file_async_busy(void) {
    /* Worst case: the read finishes right after the app checked its flag. */
    if(active&&completes_in_busy&&polls>=2)complete();
    return active;
}
uint32_t of_file_async_max_read(void){return 65536;}
#include "sd_library.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"sd async check failed at %d\n",__LINE__);exit(1);}}while(0)
static uint32_t fake_now;
static uint32_t clock_us(void){return fake_now+=50;}
int main(void) {
    static pj_sd_library s;
    for(unsigned i=0;i<sizeof(card);i++)card[i]=(uint8_t)(i*7u+3u);
    unsigned reads=0;
    for(int mode=0;mode<2;mode++) {
        completes_in_busy=mode;
        for(unsigned trial=0;trial<200;trial++) {
            static uint8_t dest[32768];
            uint32_t offset=trial*1021u%(sizeof(card)-sizeof(dest));
            polls=0;memset(dest,0,sizeof(dest));
            pj_sd_read(&s,20,offset,dest,sizeof(dest),clock_us(),PJ_SD_THEN_REPLY);
            unsigned spins=0;
            while(s.state==PJ_SD_SUBMIT||s.state==PJ_SD_READING) {
                pj_sd_pump(&s,clock_us());
                if(mode==0&&active&&spins==3)complete(); /* ordinary interrupt timing */
                CHECK(++spins<1000);
            }
            if(s.state!=PJ_SD_READY)fprintf(stderr,"mode %d trial %u: %s\n",mode,trial,s.why?s.why:"?");
            CHECK(s.state==PJ_SD_READY);
            CHECK(!memcmp(dest,card+offset,sizeof(dest)));
            s.state=PJ_SD_IDLE;reads++;
        }
    }
    printf("PASS: %u device-path card reads, including completion between the done and busy checks.\n",reads);
    return 0;
}
