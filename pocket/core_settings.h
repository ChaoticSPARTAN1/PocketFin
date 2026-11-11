/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * The Pocket menu's Core Settings for playback (interact.json; the release ships the SDK template, see
 * tools/package_release.py).
 *
 * How a choice reaches the app, with the core as it is (no FPGA rebuild). The media core is built without
 * INCLUDE_ANALOGIZER (tools/prepare_pocket_media_core.py), so of the bridge words the SDK template's menu writes
 * (0xF7000000-0xF700000C: Analogizer, offsets, mouse speed) none is stored: their write arms are inside that ifdef
 * (build/pocket-media-core/src/fpga/targets/pocket/core_top.v, the "Interact variable writes" block). One word is
 * stored in every build: 0xF7000010, the instance's app id (app_id_74a, outside the ifdef), synchronised into the CPU
 * clock and readable at sysreg 0x40000068 (axi_periph_slave.v sysreg word 26; openfpgaOS names it SYS_GAME_ID and
 * reads it nowhere). The kernel's of_interact_get() reads an SDRAM page (0x503FE000) that nothing writes on this core
 * (the bridge-to-SDRAM path was removed), so it is not used.
 *
 * The word. Four menu entries share it, each a 4-bit field, nothing else (bits 31:16 are 0):
 *   bits 3:0 frame rate, 7:4 link speed, 11:8 buffer, 15:12 volume; 0 in a field = "not given here".
 * The entries take the SDK template's form (its list entries on 0xF7000000): each option sets only its own field,
 * its mask clears exactly that field, no "writeonly", defaultval an option's value. (The 2026-11-10 form, with
 * "writeonly" plus a mask and a 0x504A tag in the high half, i.e. in bits the masks keep, stopped the Pocket at
 * "Interact: setup": black screen, flashing backlight.) With a mask the Pocket reads the word, keeps the masked bits
 * and adds the option; the core's bridge read of 0xF7000010 returns 0 (core_top.v read mux: no arm, default 0), so
 * each write carries just its own field. The app keeps every field it has seen (a field of 0 changes nothing), and
 * the merged word is also kept with the watch state (pocket/tether_store.h), so a start at which the Pocket writes
 * only one entry, or none, still has the others. Read twice, equal both times: the 32-bit word crosses clock domains
 * through plain two-flop synchronisers.
 */
#ifndef PJ_CORE_SETTINGS_H
#define PJ_CORE_SETTINGS_H
#include <stdint.h>
#include <stdio.h>
#define PJ_SETTINGS_SYSREG 0x40000068u   /* the bridge's 0xF7000010, CPU side */
enum {PJ_SET_FPS,PJ_SET_LINK,PJ_SET_BUFFER,PJ_SET_VOLUME,PJ_SET_FIELDS};
/* Field codes (0: not given). Frame rate 5 and 6 (15 and 12 fps, sharper pictures) are reserved: launcher 1.5.0 has no
 * such stream profile (shared/pocket_tether.h; an unknown name is served as its row 0), so the menu does not offer
 * them and the app plays them as 24 if a word ever carries them. */
enum {PJ_FPS_FOLLOW=1,PJ_FPS_24,PJ_FPS_30,PJ_FPS_60,PJ_FPS_15,PJ_FPS_12};
enum {PJ_LINKSET_AUTO=1,PJ_LINKSET_SAFE,PJ_LINKSET_FAST};
/* Buffer: 1 Short (0.5 s, the buffer before 2026-11-10), 2 Long (4 s), 3 Normal (2 s, the default; live_playback.h). */
enum {PJ_BUFSET_SHORT=1,PJ_BUFSET_LONG,PJ_BUFSET_NORMAL};
enum {PJ_VOLSET_NORMAL=1,PJ_VOLSET_6DB,PJ_VOLSET_12DB};
static const uint8_t pj_settings_limit[PJ_SET_FIELDS]={PJ_FPS_12,PJ_LINKSET_FAST,PJ_BUFSET_NORMAL,PJ_VOLSET_12DB};
/* The release defaults: the title's own rate (the app's 24/30/60 choice, Select), Auto link (384 kHz, the
 * 2026-11-10 ack384 test build), the Normal 2 s buffer (the model in live_playback.h: no refill in a short busy scene,
 * 1.2 s more to start than the 0.5 s of before), no gain. */
#ifndef PJ_BUFSET_DEFAULT
#define PJ_BUFSET_DEFAULT PJ_BUFSET_NORMAL
#endif
typedef struct {uint8_t code[PJ_SET_FIELDS];} pj_core_settings;
static inline pj_core_settings pj_settings_defaults(void) {
    pj_core_settings s={{PJ_FPS_FOLLOW,PJ_LINKSET_AUTO,PJ_BUFSET_DEFAULT,PJ_VOLSET_NORMAL}};return s;
}
static inline uint32_t pj_settings_swap(uint32_t v){return v>>24|(v>>8&0xff00u)|(v<<8&0xff0000u)|v<<24;}
/* The fields of a word as the app sees it: the low half holds them, the high half is 0 (the core stores the menu's
 * value as written, core_top.v undoing the bridge's byte order; the other byte order is still taken, as a word with
 * 0 in its low half). 0: not a settings word (nothing written yet, or something else). */
static inline int pj_settings_fields(uint32_t word,uint8_t code[PJ_SET_FIELDS]) {
    uint32_t v=word;
    if(v>>16) {
        uint32_t s=pj_settings_swap(v);
        if(s>>16)return 0;
        v=s;
    }
    if(!(v&0xffffu))return 0;
    /* With no tag, a code out of range in any field means the word is not one the menu wrote: none of it is taken. */
    for(unsigned f=0;f<PJ_SET_FIELDS;f++)if(((v>>(4u*f))&15u)>pj_settings_limit[f])return 0;
    for(unsigned f=0;f<PJ_SET_FIELDS;f++)code[f]=(uint8_t)((v>>(4u*f))&15u);
    return 1;
}
/* Fields given in `word` replace those in `s`; returns a bit per field that changed. */
static inline unsigned pj_settings_merge(pj_core_settings *s,uint32_t word) {
    uint8_t code[PJ_SET_FIELDS];unsigned changed=0;
    if(!pj_settings_fields(word,code))return 0;
    for(unsigned f=0;f<PJ_SET_FIELDS;f++)if(code[f]&&code[f]!=s->code[f]){s->code[f]=code[f];changed|=1u<<f;}
    return changed;
}
static inline uint32_t pj_settings_pack(const pj_core_settings *s) {
    uint32_t w=0; /* never 0 for a full set: every code is 1 or more */
    for(unsigned f=0;f<PJ_SET_FIELDS;f++)w|=(uint32_t)(s->code[f]&15u)<<(4u*f);
    return w;
}
/* The frame rate asked of the Chromatic for a fixed choice: 24, 30 or 60; 0 = follow (the app's own choice). */
static inline unsigned pj_settings_fps(const pj_core_settings *s) {
    switch(s->code[PJ_SET_FPS]){case PJ_FPS_24:case PJ_FPS_15:case PJ_FPS_12:return 24u;case PJ_FPS_30:return 30u;case PJ_FPS_60:return 60u;default:return 0u;}
}
static inline unsigned pj_settings_gain_shift(const pj_core_settings *s) {
    return s->code[PJ_SET_VOLUME]==PJ_VOLSET_12DB?2u:s->code[PJ_SET_VOLUME]==PJ_VOLSET_6DB?1u:0u;
}
/* live_playback.h PJ_LIVE_BUFFER_*: 0 Short, 1 Long, 2 Normal */
static inline unsigned pj_settings_buffer_mode(const pj_core_settings *s) {
    return s->code[PJ_SET_BUFFER]==PJ_BUFSET_SHORT?0u:s->code[PJ_SET_BUFFER]==PJ_BUFSET_LONG?1u:2u;
}
/* Seconds the buffer mode banks before playing (the app waits that much longer before "did not start"). */
static inline unsigned pj_settings_buffer_seconds(const pj_core_settings *s) {
    return s->code[PJ_SET_BUFFER]==PJ_BUFSET_SHORT?0u:s->code[PJ_SET_BUFFER]==PJ_BUFSET_LONG?4u:2u;
}
/* "30 fps, link Auto, buffer Long, +6 dB": the log's and the start screen's words. */
static inline const char *pj_settings_text(const pj_core_settings *s,char *out,size_t size) {
    static const char *const fps[]={"?","title rate","24 fps","30 fps","60 fps","15>24 fps","12>24 fps"};
    static const char *const link[]={"?","Auto 384","Safe 256","Fast 512"};
    static const char *const buffer[]={"?","Short 0.5 s","Long 4 s","Normal 2 s"};
    static const char *const volume[]={"?","0 dB","+6 dB","+12 dB"};
    snprintf(out,size,"%s, link %s, buffer %s, %s",fps[s->code[0]<=6?s->code[0]:0],link[s->code[1]<=3?s->code[1]:0],
             buffer[s->code[2]<=3?s->code[2]:0],volume[s->code[3]<=3?s->code[3]:0]);
    return out;
}
#if defined(OF_PC)
/* Host checks: the word the "menu" wrote (0: none). */
static uint32_t pj_settings_host_word;
static inline int pj_settings_read(uint32_t *word){*word=pj_settings_host_word;return 1;}
#else
static inline int pj_settings_read(uint32_t *word) {
    volatile uint32_t *reg=(volatile uint32_t *)(uintptr_t)PJ_SETTINGS_SYSREG;
    uint32_t a=*reg,b=*reg;
    if(a!=b)return 0; /* caught mid-change: next time */
    *word=a;return 1;
}
#endif
#endif
