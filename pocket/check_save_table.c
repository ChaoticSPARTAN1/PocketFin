/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * PC check for pocket/save_table.h: the app's view of the APF datatable must resolve the
 * save slots exactly as the Pocket fills the table and the kernel scans it.
 *
 * Layouts come from tools/package_release.py (passed on the command line as the
 * id list), under the compacted fill (one entry per slot with a file, data.json order)
 * and the dense fill (one entry per declared slot). Also: an absent slot, a query path that
 * stops answering, and a stale duplicate id above the real entry (the lowest entry wins,
 * like the kernel).   gcc -std=c11 -DOF_PC check_save_table.c
 */
#define OF_PC 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "save_table.h"

static uint32_t table[2*PJ_DT_ENTRIES];
static int fail_after=-1,reads;
static int read_word(void *context,uint32_t word,uint32_t *value) {
    (void)context;
    if(fail_after>=0&&reads>=fail_after)return -1;
    reads++;
    if(word>=2*PJ_DT_ENTRIES)return -1;
    *value=table[word];return 0;
}
static int failures;
static void check(const char *what,int ok){printf("%s %s\n",ok?"OK  ":"FAIL",what);failures+=!ok;}

int main(int argc,char **argv) {
    /* argv: ids in table order, e.g. 0 1 2 3 4 20 10 11 12 ... 19 (a one-pack card) */
    unsigned ids[PJ_DT_ENTRIES],count=0;
    for(int i=1;i<argc&&count<PJ_DT_ENTRIES;i++)ids[count++]=(unsigned)strtoul(argv[i],NULL,0);
    if(count<3){fprintf(stderr,"usage: check_save_table <slot ids in table order>\n");return 2;}
    for(unsigned e=0;e<count;e++){table[2*e]=ids[e];table[2*e+1]=(ids[e]>=8&&ids[e]<=19)?32768u:0x1000u+ids[e];}
    pj_dt_entry got;
    int expect10=-1,expect11=-1;
    for(unsigned e=0;e<count;e++){if(ids[e]==10)expect10=(int)e;if(ids[e]==11)expect11=(int)e;}
    check("slot 10 found at its entry",pj_dt_find(read_word,NULL,10,&got)==1&&got.entry==expect10&&got.size==32768u);
    check("slot 11 found at its entry",pj_dt_find(read_word,NULL,11,&got)==1&&got.entry==expect11&&got.size==32768u);
    /* The kernel's close() commit changes only the size word; the app must read the new size. */
    table[2*(unsigned)expect10+1]=1234u;
    check("committed size read back",pj_dt_find(read_word,NULL,10,&got)==1&&got.size==1234u);
    /* A stale duplicate id above the real entry must not preempt it (lowest entry wins). */
    table[2*(count+2)]=10u;table[2*(count+2)+1]=7u;
    check("lowest entry wins over a stale duplicate",pj_dt_find(read_word,NULL,10,&got)==1&&got.entry==expect10&&got.size==1234u);
    check("absent slot reports 0",pj_dt_find(read_word,NULL,9,&got)==0&&got.entry==-1&&got.size==0);
    fail_after=3;reads=0;
    check("query path failure reports -1",pj_dt_find(read_word,NULL,11,&got)==-1);
    fail_after=-1;
    printf(count?"%s\n":"","entries checked");
    printf(failures?"%d FAILED\n":"PASS\n",failures);
    return failures?1:0;
}
