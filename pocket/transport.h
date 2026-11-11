/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * One request/reply contract for every media source. The app, library model, UI
 * and playback engine only ever see this, so they are written once and shared by
 * the on-card library (sd_library.h) and the tethered bridge (link_transport.h).
 * Replies are the same PJB1 payload bytes the PC Gateway produces.
 */
#ifndef PJ_TRANSPORT_H
#define PJ_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
#define PJ_POLL_READY 1
#define PJ_POLL_WAITING 0
#define PJ_POLL_FAILED (-1)   /* source lost: fall back to the source screen */
#define PJ_POLL_REFUSED (-2)  /* source is healthy but cannot serve this request */
typedef struct {
    void *context;
    const char *label;  /* shown in the header */
    int stored;         /* 1: content lives on this card; 0: live over a link */
    int (*idle)(void *);
    int (*begin)(void *,unsigned command,const char *json,uint32_t now_us);
    int (*poll)(void *,uint32_t now_us);
    const uint8_t *(*reply)(void *,size_t *size);
    void (*close)(void *);
} pj_transport;
#endif
