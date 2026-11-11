/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Playback diagnostics kept in RAM and copied into the app's save slot (APF slot 10).
 * The Pocket writes that slot to the card as Saves/pocketlive/.../pjlive_log.sav when the
 * core is quit from the Analogue menu; it is plain text that can be read on the PC (bug
 * reports may attach it, see CONTRIBUTING.md).
 *
 * Content is numbers and short fixed reasons only: no titles, no server details, nothing
 * from the connection file.
 *
 * Saving hands the save memory (CRAM0) to the CPU, which the card bridge also uses for
 * reads, so the caller must only save while no card read is in flight or being copied
 * (see save_log() in live_player.c). The slot is opened as "slot:10", which the OS always
 * maps to the save slot even when the card has no save file yet.
 */
#ifndef PJ_DIAG_LOG_H
#define PJ_DIAG_LOG_H
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PJ_DIAG_BYTES 24576u      /* well under the 256 KB save slot */
#define PJ_DIAG_SLOT_ID 10u
#define PJ_DIAG_SLOT "slot:10"     /* APF save slot 10; tools/package_release.py LOG_SLOT */
#define PJ_DIAG_MAGIC "PJLIVE-LOG 1\n"
#define PJ_DIAG_KEEP 6144u        /* previous run's newest text kept at start-up */

typedef struct {char text[PJ_DIAG_BYTES];size_t used;uint32_t dropped,saves,boot;int dirty;
    int err,stage; /* last failure: errno and where (1 open, 2 write, 3 close), for the start screen */} pj_diag_log;

#if defined(__GNUC__)
#define PJ_DIAG_PRINTF __attribute__((format(printf,3,4)))
#else
#define PJ_DIAG_PRINTF
#endif
static inline void pj_diag_raw(pj_diag_log *l,const char *text,size_t n) {
    if(n>PJ_DIAG_BYTES/2)n=PJ_DIAG_BYTES/2;
    while(l->used+n>PJ_DIAG_BYTES) {
        /* Drop the oldest quarter, cut at a line end, so the newest text survives. */
        size_t cut=PJ_DIAG_BYTES/4;
        if(cut>l->used)cut=l->used;
        while(cut<l->used&&l->text[cut-1]!='\n')cut++;
        memmove(l->text,l->text+cut,l->used-cut);l->used-=cut;l->dropped++;
    }
    memcpy(l->text+l->used,text,n);l->used+=n;l->dirty=1;
}
static inline void pj_diag_add(pj_diag_log *l,uint32_t now_us,const char *fmt,...) PJ_DIAG_PRINTF;
static inline void pj_diag_add(pj_diag_log *l,uint32_t now_us,const char *fmt,...) {
    char line[200];
    int head=snprintf(line,sizeof(line),"%lu.%03lu ",(unsigned long)(now_us/1000000u),
                      (unsigned long)(now_us/1000u%1000u));
    if(head<0)return;
    va_list args;va_start(args,fmt);
    int body=vsnprintf(line+head,sizeof(line)-(size_t)head-1,fmt,args);
    va_end(args);
    if(body<0)return;
    size_t n=strlen(line);
    for(size_t i=0;i<n;i++)if((unsigned char)line[i]<0x20)line[i]=' ';
    line[n++]='\n';
    pj_diag_raw(l,line,n);
}
/* At start-up, before anything else is saved: keep the newest part of the previous run's
 * log (a freeze leaves no other record) and continue its boot count. 1 if one was found. */
static inline int pj_diag_resume(pj_diag_log *l) {
#if defined(OF_PC)&&!defined(PJ_DIAG_PC_SAVE)
    l->boot=1;return 0;
#else
    static char old[PJ_DIAG_BYTES+128];
    size_t n=0;
    FILE *f=fopen(PJ_DIAG_SLOT,"rb");
    if(f){n=fread(old,1,sizeof(old)-1,f);fclose(f);}
    old[n]=0;
    size_t magic=sizeof(PJ_DIAG_MAGIC)-1;
    unsigned long boot=0;
    if(n<magic||memcmp(old,PJ_DIAG_MAGIC,magic)){l->boot=1;return 0;}
    char *line=old+magic,*body=strchr(line,'\n');
    if(!body){l->boot=1;return 0;}
    if(sscanf(line,"boot %lu",&boot)!=1)boot=0;
    l->boot=(uint32_t)boot+1;body++;
    size_t len=strlen(body);
    const char *from=body;
    if(len>PJ_DIAG_KEEP) {
        from=body+len-PJ_DIAG_KEEP;
        while(*from&&*from!='\n')from++;
        if(*from)from++;
    }
    char note[64];
    int h=snprintf(note,sizeof(note),"--- end of run %lu (newest lines) ---\n",boot);
    if(h>0){pj_diag_raw(l,note,(size_t)h);}
    pj_diag_raw(l,from,strlen(from));
    h=snprintf(note,sizeof(note),"--- run %lu ---\n",(unsigned long)l->boot);
    if(h>0)pj_diag_raw(l,note,(size_t)h);
    return 1;
#endif
}
/* Copies the log into the save slot; 1 when written. Opening with "wb" truncates and
 * closing commits the real length, so no padding is needed. */
static inline int pj_diag_save(pj_diag_log *l) {
    if(!l->dirty)return 1;
#if defined(OF_PC)&&!defined(PJ_DIAG_PC_SAVE)
    l->dirty=0;l->saves++;return 1; /* desktop checks keep the log in memory only */
#else
    errno=0;FILE *f=fopen(PJ_DIAG_SLOT,"wb");
    if(!f){l->err=errno;l->stage=1;return 0;}
    char head[80];
    int h=snprintf(head,sizeof(head),"%sboot %lu dropped %lu save %lu\n",PJ_DIAG_MAGIC,
                   (unsigned long)l->boot,(unsigned long)l->dropped,(unsigned long)(l->saves+1));
    int ok=h>0&&fwrite(head,1,(size_t)h,f)==(size_t)h&&fwrite(l->text,1,l->used,f)==l->used;
    if(!ok){l->err=errno;l->stage=2;}
    if(fclose(f)!=0&&ok){ok=0;l->err=errno;l->stage=3;}
    if(ok){l->dirty=0;l->saves++;}
    return ok;
#endif
}
#endif
