/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Binding used by the future native RPC client. No of_net calls: its 33-bit
 * framing is incompatible. Caller supplies a framebuffer/HUD present callback.
 */
#ifndef PJ_LIVE_POCKET_BACKEND_H
#define PJ_LIVE_POCKET_BACKEND_H
#include "live_playback.h"
#include "../shared/pocket_jpeg.h"
#include "of_audio.h"
typedef struct {
    pj_pocket_jpeg jpeg;
    void (*display)(const uint16_t *,unsigned,unsigned,uint64_t);
} pj_live_pocket;
static void pj_live_pocket_reset_audio(void *context){(void)context;of_audio_init();}
static unsigned pj_live_pocket_audio_free(void *context) {
    (void)context;int count=of_audio_free();return count>0?(unsigned)count:0;
}
static unsigned pj_live_pocket_audio_write(void *context,const int16_t *samples,unsigned count) {
    (void)context;int n=of_audio_write(samples,(int)count);return n>0?(unsigned)n:0;
}
static int pj_live_pocket_begin(void *context,const uint8_t *data,size_t size,uint16_t *frame,
                               unsigned width,unsigned height,uint32_t now) {
    return pj_jpeg_begin(&((pj_live_pocket *)context)->jpeg,data,size,frame,width,height,now);
}
static int pj_live_pocket_poll(void *context,unsigned budget,uint32_t now) {
    return pj_jpeg_service(&((pj_live_pocket *)context)->jpeg,budget,now);
}
static void pj_live_pocket_abort(void *context){pj_jpeg_abort(&((pj_live_pocket *)context)->jpeg);}
static void pj_live_pocket_present(void *context,const uint16_t *frame,unsigned w,unsigned h,uint64_t pts) {
    ((pj_live_pocket *)context)->display(frame,w,h,pts);
}
static int pj_live_pocket_ops(pj_live_pocket *context,pj_live_ops *ops,
    void (*display)(const uint16_t *,unsigned,unsigned,uint64_t)) {
    if(!context||!ops||!display||!pj_jpeg_open(&context->jpeg,(volatile uint32_t *)(uintptr_t)PJ_JPEG_BASE))return 0;
    context->display=display;
    *ops=(pj_live_ops){context,pj_live_pocket_reset_audio,pj_live_pocket_audio_free,pj_live_pocket_audio_write,
        pj_live_pocket_begin,pj_live_pocket_poll,pj_live_pocket_abort,pj_live_pocket_present};
    return 1;
}
#endif
