/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Native UI following the Jellyfin client's visual language at 320x240: dark page,
 * backdrop hero fading into it, labelled rows of 16:9 thumbs and 2:3 posters,
 * accent progress bars and watched ticks, details page with overview and actions.
 * Pure drawing: state in, pixels out. The same code renders on the Pocket and in
 * the PC preview. The logo is Jellyfin's own (ui_brand.h, CC BY-SA 4.0). */
#ifndef PJ_LIVE_UI_H
#define PJ_LIVE_UI_H
#include <stdio.h>
#include "live_library.h"
#include "ui_font.h"
#include "ui_font_large.h"
#include "ui_brand.h"
#define PJ_FB_W 320
#define PJ_FB_H 240
#define PJ_UI_PAGE 0x101010u    /* page background */
#define PJ_UI_CARD 0x202020u    /* empty card surface */
#define PJ_UI_ACCENT 0x00a4dcu  /* primary accent */
#define PJ_UI_PURPLE 0xaa5cc3u
#define PJ_UI_INK 0xffffffu
#define PJ_UI_MUTED 0xb3b3b3u
#define PJ_UI_FOCUS 0xe0e0e0u
#define PJ_ART_CARD_SLOTS 32u
#define PJ_ART_BACKDROP_SLOTS 4u
#define PJ_ART_CARD_PIXELS (PJ_ART_POSTER_W*PJ_ART_POSTER_H)

typedef struct {uint32_t handle,stamp;uint8_t kind,present,used;uint16_t pixels[PJ_ART_CARD_PIXELS];} pj_art_card;
typedef struct {uint32_t handle,stamp;uint8_t present,used;uint16_t pixels[PJ_ART_BACKDROP_W*PJ_ART_BACKDROP_H];} pj_art_hero;
typedef struct {pj_art_card cards[PJ_ART_CARD_SLOTS];pj_art_hero heroes[PJ_ART_BACKDROP_SLOTS];uint32_t clock;} pj_art_cache;
enum {PJ_SCREEN_SOURCE,PJ_SCREEN_HOME,PJ_SCREEN_LIBRARY,PJ_SCREEN_DETAILS,PJ_SCREEN_CREDITS};
/* The author's public handle, as in core.json (tools/package_release.py). */
#define PJ_UI_CREATED_BY "Created by ChaoticSPARTAN1"
typedef struct {
    int screen,paused;unsigned selected,action;
    const pj_live_library *library;pj_art_cache *art;const pj_details *details;
    const char *status,*source,*quality;uint32_t position_seconds;
    const char *diagnostics[5];int can_stored,can_tether;
    /* Tether signs (live_player.c): a plain-words line in the header of every library screen
     * ("Chromatic found: syncing 4 of 18"), with a progress bar while total is nonzero. On the
     * start screen the sign is the big guide card and `hint` its second line. */
    const char *sign,*hint;unsigned progress_done,progress_total;int sign_tone;
    const char *unavailable_note; /* Details, for a dimmed title; default: the PC sync hint */
    const char *countdown;        /* Details: next episode countdown, e.g. "Next episode in 5" */
    const char *countdown_next;   /* ... and what plays next, e.g. "S1:E2 - The Night Ferry" */
    const char *offline_label,*alternate_label; /* start screen buttons (default: PC library) */
    /* Start screen, the Chromatic link in numbers (live_player.c link_text): rate and search
     * step, bytes and frames, faults. Shown first, above the log line and the other checks. */
    const char *link[3];
} pj_ui_state;
enum {PJ_SIGN_INFO,PJ_SIGN_BUSY,PJ_SIGN_DONE,PJ_SIGN_WARN};

/* Evict the oldest entry; a card never holds two copies of the same art. */
static inline pj_art_card *pj_art_find_card(pj_art_cache *c,uint32_t handle,unsigned kind) {
    for(unsigned i=0;i<PJ_ART_CARD_SLOTS;i++)
        if(c->cards[i].used&&c->cards[i].handle==handle&&c->cards[i].kind==kind) {
            c->cards[i].stamp=++c->clock;return &c->cards[i];
        }
    return NULL;
}
static inline pj_art_hero *pj_art_find_hero(pj_art_cache *c,uint32_t handle) {
    for(unsigned i=0;i<PJ_ART_BACKDROP_SLOTS;i++)
        if(c->heroes[i].used&&c->heroes[i].handle==handle) {
            c->heroes[i].stamp=++c->clock;return &c->heroes[i];
        }
    return NULL;
}
static inline void pj_art_store(pj_art_cache *c,const pj_art_download *a) {
    if(a->kind==PJ_ART_BACKDROP) {
        pj_art_hero *best=&c->heroes[0];
        for(unsigned i=0;i<PJ_ART_BACKDROP_SLOTS;i++) {
            if(!c->heroes[i].used){best=&c->heroes[i];break;}
            if(c->heroes[i].stamp<best->stamp)best=&c->heroes[i];
        }
        best->handle=a->handle;best->used=1;best->present=a->total!=0;best->stamp=++c->clock;
        if(a->total)memcpy(best->pixels,a->bytes,a->total);
        return;
    }
    pj_art_card *best=&c->cards[0];
    for(unsigned i=0;i<PJ_ART_CARD_SLOTS;i++) {
        if(!c->cards[i].used){best=&c->cards[i];break;}
        if(c->cards[i].stamp<best->stamp)best=&c->cards[i];
    }
    best->handle=a->handle;best->kind=(uint8_t)a->kind;best->used=1;best->present=a->total!=0;
    best->stamp=++c->clock;
    if(a->total)memcpy(best->pixels,a->bytes,a->total);
}

static inline uint16_t pj_ui_rgb(unsigned rgb) {
    return (uint16_t)(((rgb>>8)&0xf800u)|((rgb>>5)&0x07e0u)|((rgb>>3)&31u));
}
static inline void pj_ui_rect(uint16_t *fb,int x,int y,int w,int h,unsigned rgb) {
    uint16_t color=pj_ui_rgb(rgb);
    int x1=x+w,y1=y+h;
    if(x<0)x=0;
    if(y<0)y=0;
    if(x1>PJ_FB_W)x1=PJ_FB_W;
    if(y1>PJ_FB_H)y1=PJ_FB_H;
    for(int row=y;row<y1;row++)for(int col=x;col<x1;col++)fb[row*PJ_FB_W+col]=color;
}
static inline void pj_ui_blend(uint16_t *fb,int px,int py,unsigned rgb,unsigned a) {
    unsigned old=fb[py*PJ_FB_W+px];
    unsigned r=(((rgb>>16)&255)*a+((old>>11)*255/31)*(255-a)+127)/255;
    unsigned g=(((rgb>>8)&255)*a+(((old>>5)&63)*255/63)*(255-a)+127)/255;
    unsigned b=((rgb&255)*a+((old&31)*255/31)*(255-a)+127)/255;
    fb[py*PJ_FB_W+px]=pj_ui_rgb((r<<16)|(g<<8)|b);
}
static inline int pj_ui_glyph(const char *text,unsigned *index) {
    unsigned char c=(unsigned char)text[*index];
    if(c>=128) { /* one '?' per UTF-8 sequence: the atlas is ASCII */
        (*index)++;
        while((((unsigned char)text[*index])&0xc0)==0x80)(*index)++;
        return '?'-32;
    }
    (*index)++;
    return (c<32||c>126?' ':c)-32;
}
static inline int pj_ui_measure(const char *text) {
    int width=0;
    for(unsigned i=0;text[i];)width+=pj_font_advance[pj_ui_glyph(text,&i)];
    return width;
}
static inline int pj_ui_measure_large(const char *text) {
    int width=0;
    for(unsigned i=0;text[i];)width+=pj_font_large_advance[pj_ui_glyph(text,&i)];
    return width;
}
/* Returns the pen position after drawing; clips at maxwidth with an ellipsis. */
static inline int pj_ui_text(uint16_t *fb,int x,int y,const char *text,unsigned rgb,int maxwidth) {
    int right=x+maxwidth,fits=pj_ui_measure(text)<=maxwidth;
    int limit=fits?right:right-3*pj_font_advance['.'-32];
    for(unsigned i=0;text[i];) {
        int glyph=pj_ui_glyph(text,&i),advance=pj_font_advance[glyph];
        if(x+advance>limit)break;
        for(int row=0;row<11;row++)for(int col=0;col<13;col++) {
            int px=x+col,py=y+row;
            if(px<0||px>=PJ_FB_W||px>=right||py<0||py>=PJ_FB_H)continue;
            unsigned a=pj_font_alpha[(unsigned)glyph*143u+(unsigned)row*13u+(unsigned)col];
            if(a)pj_ui_blend(fb,px,py,rgb,a);
        }
        x+=advance;
    }
    if(!fits)for(int dot=0;dot<3;dot++) {
        int glyph='.'-32;
        for(int row=0;row<11;row++)for(int col=0;col<13;col++) {
            int px=x+col,py=y+row;
            if(px<0||px>=PJ_FB_W||py<0||py>=PJ_FB_H)continue;
            unsigned a=pj_font_alpha[(unsigned)glyph*143u+(unsigned)row*13u+(unsigned)col];
            if(a)pj_ui_blend(fb,px,py,rgb,a);
        }
        x+=pj_font_advance[glyph];
    }
    return x;
}
static inline void pj_ui_title(uint16_t *fb,int x,int y,const char *text,unsigned rgb,int maxwidth) {
    int right=x+maxwidth;
    for(unsigned i=0;text[i];) {
        int glyph=pj_ui_glyph(text,&i),advance=pj_font_large_advance[glyph];
        if(x+advance>right)return;
        for(int row=0;row<PJ_FONT_LARGE_H;row++)for(int col=0;col<PJ_FONT_LARGE_W;col++) {
            int px=x+col-PJ_FONT_LARGE_OX,py=y+row;
            if(px<0||px>=PJ_FB_W||py<0||py>=PJ_FB_H)continue;
            unsigned a=pj_font_large[(unsigned)glyph*PJ_FONT_LARGE_W*PJ_FONT_LARGE_H+
                                     (unsigned)row*PJ_FONT_LARGE_W+(unsigned)col];
            if(a)pj_ui_blend(fb,px,py,rgb,a);
        }
        x+=advance;
    }
}
static inline void pj_ui_shadow_title(uint16_t *fb,int x,int y,const char *text,int maxwidth) {
    pj_ui_title(fb,x+1,y+1,text,0x000000u,maxwidth);pj_ui_title(fb,x,y,text,PJ_UI_INK,maxwidth);
}
/* The large atlas resampled to num/den of its size (bilinear on glyph coverage): the Home
 * hero title, a step below the Details title without a third font. */
static inline void pj_ui_title_scaled(uint16_t *fb,int x,int y,const char *text,unsigned rgb,int maxwidth,
                                      unsigned num,unsigned den) {
    const int ow=(int)((PJ_FONT_LARGE_W*num+den-1u)/den),oh=(int)((PJ_FONT_LARGE_H*num+den-1u)/den);
    const int ox0=(int)((PJ_FONT_LARGE_OX*num+den/2u)/den);
    uint32_t pen=0; /* 24.8 fixed point */
    for(unsigned i=0;text[i];) {
        int glyph=pj_ui_glyph(text,&i);
        uint32_t step=(uint32_t)pj_font_large_advance[glyph]*num*256u/den;
        if((int)((pen+step)>>8)>maxwidth)return;
        const unsigned char *g=pj_font_large+(unsigned)glyph*PJ_FONT_LARGE_W*PJ_FONT_LARGE_H;
        int gx=x+(int)(pen>>8)-ox0;
        for(int oy=0;oy<oh;oy++) {
            int sy=(int)(((2u*(unsigned)oy+1u)*den*128u)/num)-128,y0=sy>=0?sy>>8:-1,fy=sy&255;
            for(int ox=0;ox<ow;ox++) {
                int sx=(int)(((2u*(unsigned)ox+1u)*den*128u)/num)-128,x0=sx>=0?sx>>8:-1,fx=sx&255;
                unsigned tap[4]={0,0,0,0};
                for(int k=0;k<4;k++) {
                    int cx=x0+(k&1),cy=y0+(k>>1);
                    if(cx>=0&&cx<PJ_FONT_LARGE_W&&cy>=0&&cy<PJ_FONT_LARGE_H)tap[k]=g[cy*PJ_FONT_LARGE_W+cx];
                }
                unsigned a=((tap[0]*(256u-(unsigned)fx)+tap[1]*(unsigned)fx)*(256u-(unsigned)fy)+
                            (tap[2]*(256u-(unsigned)fx)+tap[3]*(unsigned)fx)*(unsigned)fy)>>16;
                int px=gx+ox,py=y+oy;
                if(a&&px>=0&&px<PJ_FB_W&&px<x+maxwidth&&py>=0&&py<PJ_FB_H)pj_ui_blend(fb,px,py,rgb,a>255u?255u:a);
            }
        }
        pen+=step;
    }
}
static inline void pj_ui_shadow_text(uint16_t *fb,int x,int y,const char *text,unsigned rgb,int maxwidth) {
    pj_ui_text(fb,x+1,y+1,text,0x000000u,maxwidth);pj_ui_text(fb,x,y,text,rgb,maxwidth);
}
/* A hue picked from the handle gives empty cards the app's coloured placeholders. */
static inline unsigned pj_ui_placeholder(uint32_t handle) {
    static const unsigned colors[5]={0x1b6a8a,0x6b3f7a,0x3f6b4a,0x7a4a2f,0x2f4a7a};
    return colors[handle%5u];
}
static inline void pj_ui_blit(uint16_t *fb,int x,int y,const uint16_t *pixels,unsigned w,unsigned h) {
    for(unsigned row=0;row<h;row++) {
        int py=y+(int)row;
        if(py<0||py>=PJ_FB_H)continue;
        for(unsigned col=0;col<w;col++) {
            int px=x+(int)col;
            if(px>=0&&px<PJ_FB_W)fb[py*PJ_FB_W+px]=pixels[row*w+col];
        }
    }
}
/* Rounded corners: repaint the two outermost pixels of each corner with the page. */
static inline void pj_ui_round(uint16_t *fb,int x,int y,int w,int h,unsigned rgb) {
    uint16_t c=pj_ui_rgb(rgb);
    static const signed char dx[3]={0,1,0},dy[3]={0,0,1};
    for(unsigned i=0;i<3;i++) {
        int points[4][2]={{x+dx[i],y+dy[i]},{x+w-1-dx[i],y+dy[i]},{x+dx[i],y+h-1-dy[i]},{x+w-1-dx[i],y+h-1-dy[i]}};
        for(unsigned p=0;p<4;p++)
            if(points[p][0]>=0&&points[p][0]<PJ_FB_W&&points[p][1]>=0&&points[p][1]<PJ_FB_H)
                fb[points[p][1]*PJ_FB_W+points[p][0]]=c;
    }
}
static inline void pj_ui_outline(uint16_t *fb,int x,int y,int w,int h,int t,unsigned rgb) {
    pj_ui_rect(fb,x,y,w,t,rgb);pj_ui_rect(fb,x,y+h-t,w,t,rgb);
    pj_ui_rect(fb,x,y,t,h,rgb);pj_ui_rect(fb,x+w-t,y,t,h,rgb);
}
static inline void pj_ui_tick(uint16_t *fb,int x,int y) {
    pj_ui_rect(fb,x,y,12,12,PJ_UI_ACCENT);pj_ui_round(fb,x,y,12,12,PJ_UI_ACCENT);
    static const signed char tick[][2]={{3,6},{4,7},{5,8},{6,7},{7,6},{8,5},{9,4}};
    for(unsigned i=0;i<7;i++) {
        fb[(y+tick[i][1])*PJ_FB_W+x+tick[i][0]]=pj_ui_rgb(PJ_UI_INK);
        fb[(y+tick[i][1]-1)*PJ_FB_W+x+tick[i][0]]=pj_ui_rgb(PJ_UI_INK);
    }
}
static inline void pj_ui_dim(uint16_t *fb,int x,int y,int w,int h) {
    for(int row=y;row<y+h;row++)for(int col=x;col<x+w;col++) {
        if(row<0||row>=PJ_FB_H||col<0||col>=PJ_FB_W)continue;
        fb[row*PJ_FB_W+col]=(uint16_t)((fb[row*PJ_FB_W+col]>>1)&0x7befu);
    }
}
static inline void pj_ui_card(uint16_t *fb,int x,int y,const pj_library_item *item,unsigned kind,
                             pj_art_cache *cache,int focused) {
    int w=(int)pj_art_width(kind),h=(int)pj_art_height(kind);
    pj_art_card *art=pj_art_find_card(cache,item->handle,kind);
    if(art&&art->present)pj_ui_blit(fb,x,y,art->pixels,(unsigned)w,(unsigned)h);
    else {
        /* The app's "On this Pocket" tile has no picture: it is always drawn as a named card. */
        pj_ui_rect(fb,x,y,w,h,art||item->card_tile?pj_ui_placeholder(item->handle):PJ_UI_CARD);
        if(art||item->card_tile)pj_ui_text(fb,x+5,y+h/2-5,item->title,PJ_UI_INK,w-10);
    }
    if(item->unavailable)pj_ui_dim(fb,x,y,w,h);
    if(item->duration_ticks&&item->resume_ticks&&!item->played) {
        int filled=(int)((uint64_t)(w-8)*item->resume_ticks/item->duration_ticks);
        pj_ui_rect(fb,x+4,y+h-6,w-8,3,0x404040u);
        pj_ui_rect(fb,x+4,y+h-6,filled>w-8?w-8:filled,3,PJ_UI_ACCENT);
    }
    if(item->played)pj_ui_tick(fb,x+w-15,y+3);
    if(focused) {
        pj_ui_outline(fb,x-2,y-2,w+4,h+4,2,PJ_UI_FOCUS);
        pj_ui_round(fb,x-2,y-2,w+4,h+4,PJ_UI_PAGE);
    } else pj_ui_round(fb,x,y,w,h,PJ_UI_PAGE);
}
/* Darken the picture and fade its base into the page, so text always reads. scrim_top >= 0
 * (Home) also darkens it from that row down to about 14% behind the title, so lettering that
 * is part of the artwork (logos, library names) never collides with the title drawn over it. */
static inline void pj_ui_hero(uint16_t *fb,const pj_art_hero *hero,int height,int scrim_top) {
    uint16_t page=pj_ui_rgb(PJ_UI_PAGE);
    const int fade=44,ramp=28;
    if(!hero||!hero->present) {
        for(int y=0;y<height;y++) { /* a quiet accent wash, easing into the page */
            unsigned lift=(unsigned)(height-y)*28u/(unsigned)height;
            pj_ui_rect(fb,0,y,PJ_FB_W,1,((0x10u+lift/3)<<16)|((0x10u+lift*2/3)<<8)|(0x10u+lift));
        }
        return;
    }
    for(int y=0;y<height;y++) {
        /* 60% brightness at the top, reaching the page colour at the bottom. */
        unsigned keep=y<height-fade?154u:(unsigned)(height-y)*154u/(unsigned)fade;
        if(scrim_top>=0&&y>=scrim_top) {
            unsigned dark=y>=scrim_top+ramp?36u:154u-(unsigned)(y-scrim_top)*118u/(unsigned)ramp;
            if(dark<keep)keep=dark;
        }
        unsigned rest=256u-keep;
        unsigned pr=(unsigned)(page>>11)*rest,pg=(unsigned)((page>>5)&63u)*rest,pb=(unsigned)(page&31u)*rest;
        for(int x=0;x<PJ_FB_W;x++) {
            unsigned v=hero->pixels[y*PJ_FB_W+x];
            fb[y*PJ_FB_W+x]=(uint16_t)((((v>>11)*keep+pr)>>8)<<11|((((v>>5)&63u)*keep+pg)>>8)<<5|(((v&31u)*keep+pb)>>8));
        }
    }
}
/* Alpha-blends an RGB565 image; the logo sits over backdrops as well as the page. */
static inline void pj_ui_image(uint16_t *fb,int x0,int y0,int w,int h,const uint16_t *rgb,const uint8_t *alpha) {
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        unsigned a=alpha[y*w+x];int fx=x0+x,fy=y0+y;
        if(!a||fx<0||fy<0||fx>=PJ_FB_W||fy>=PJ_FB_H)continue;
        unsigned s=rgb[y*w+x],d=fb[fy*PJ_FB_W+fx],k=255-a;
        unsigned r=((s>>11)*a+(d>>11)*k)/255,g=(((s>>5)&63u)*a+((d>>5)&63u)*k)/255,b=((s&31u)*a+(d&31u)*k)/255;
        fb[fy*PJ_FB_W+fx]=(uint16_t)(r<<11|g<<5|b);
    }
}
static inline unsigned pj_ui_tone(int tone) {
    return tone==PJ_SIGN_WARN?0xf2b450u:tone==PJ_SIGN_DONE?0x7cd992u:tone==PJ_SIGN_BUSY?0x5cc8f0u:PJ_UI_MUTED;
}
/* A small filled dot in the sign's colour, the "light" before every sign. */
static inline void pj_ui_dot(uint16_t *fb,int x,int y,unsigned rgb) {
    pj_ui_rect(fb,x+1,y,4,6,rgb);pj_ui_rect(fb,x,y+1,6,4,rgb);
}
static inline void pj_ui_header(uint16_t *fb,const char *source,const char *quality) {
    pj_ui_image(fb,9,6,PJ_BRAND_MARK_W,PJ_BRAND_MARK_H,pj_brand_mark_rgb,pj_brand_mark_alpha);
    char label[48];
    snprintf(label,sizeof(label),"%s%s%s",source?source:"",quality&&quality[0]?"  ":"",quality?quality:"");
    int width=pj_ui_measure(label);
    pj_ui_shadow_text(fb,PJ_FB_W-10-width,9,label,PJ_UI_MUTED,width+1);
}
/* The header with a sign in place of the source badge: dot, words, and a thin progress bar. */
static inline void pj_ui_header_state(uint16_t *fb,const pj_ui_state *s) {
    if(!s->sign||!s->sign[0]){pj_ui_header(fb,s->source,s->quality);return;}
    pj_ui_image(fb,9,6,PJ_BRAND_MARK_W,PJ_BRAND_MARK_H,pj_brand_mark_rgb,pj_brand_mark_alpha);
    unsigned rgb=pj_ui_tone(s->sign_tone);
    int room=PJ_FB_W-10-(9+PJ_BRAND_MARK_W+12)-10,width=pj_ui_measure(s->sign);
    if(width>room)width=room;
    int x=PJ_FB_W-10-width;
    pj_ui_dot(fb,x-10,12,rgb);
    pj_ui_shadow_text(fb,x,9,s->sign,PJ_UI_INK,width+1);
    if(s->progress_total) {
        unsigned done=s->progress_done>s->progress_total?s->progress_total:s->progress_done;
        pj_ui_rect(fb,x-10,23,width+10,2,0x303030u);pj_ui_rect(fb,x-10,23,(int)((uint64_t)(width+10)*done/s->progress_total),2,rgb);
    }
}
static inline void pj_ui_duration(char *out,size_t size,uint64_t ticks) {
    unsigned minutes=(unsigned)(ticks/600000000u);
    if(!minutes)out[0]=0;
    else if(minutes<60)snprintf(out,size,"%um",minutes);
    else snprintf(out,size,"%uh %um",minutes/60,minutes%60);
}
static inline void pj_ui_meta(char *out,size_t size,const pj_library_item *item) {
    char duration[16],year[8]="";pj_ui_duration(duration,sizeof(duration),item->duration_ticks);
    if(item->year)snprintf(year,sizeof(year),"%u",(unsigned)item->year);
    snprintf(out,size,"%s%s%s%s%s",year,year[0]&&duration[0]?"   ":"",duration,
             (year[0]||duration[0])&&item->rating[0]?"   ":"",item->rating);
}
static inline unsigned pj_ui_focused_row(const pj_live_library *m,unsigned selected) {
    for(unsigned i=0;i<m->row_count;i++)
        if(selected>=m->rows[i].first&&selected<m->rows[i].first+m->rows[i].count)return i;
    return 0;
}
/* Small credit on the bottom edge of Home, right-aligned below the cards (posters end at
 * row 222 with the focus ring). Returns the room left of it for a status line. */
static inline int pj_ui_home_credit(uint16_t *fb) {
    int width=pj_ui_measure(PJ_UI_CREATED_BY),x=PJ_FB_W-10-width;
    pj_ui_text(fb,x,PJ_FB_H-15,PJ_UI_CREATED_BY,0x6a6a6au,width+1);
    return x-12-12;
}
static inline void pj_ui_home(uint16_t *fb,const pj_ui_state *s) {
    const pj_live_library *m=s->library;
    pj_ui_rect(fb,0,0,PJ_FB_W,PJ_FB_H,PJ_UI_PAGE);
    if(!m->count) {
        pj_ui_hero(fb,NULL,120,-1);pj_ui_header_state(fb,s);
        pj_ui_text(fb,16,120,s->status,PJ_UI_INK,288);pj_ui_home_credit(fb);return;
    }
    const pj_library_item *item=&m->items[s->selected];
    /* Title low in the hero, a step smaller (5/6 of the large font), on a dark scrim (
     * higher up it collided with lettering inside the artwork). */
    pj_ui_hero(fb,pj_art_find_hero(s->art,item->handle),120,30);
    pj_ui_header_state(fb,s);
    pj_ui_title_scaled(fb,13,63,item->title,0x000000u,296,5,6);
    pj_ui_title_scaled(fb,12,62,item->title,PJ_UI_INK,296,5,6);
    char meta[96];
    if(item->subtitle[0])snprintf(meta,sizeof(meta),"%s",item->subtitle);
    else pj_ui_meta(meta,sizeof(meta),item);
    pj_ui_shadow_text(fb,12,83,meta,PJ_UI_MUTED,296);
    unsigned focused=pj_ui_focused_row(m,s->selected);
    const pj_library_row *row=&m->rows[focused];
    pj_ui_text(fb,12,107,row->title,PJ_UI_INK,230);
    if(focused+1<m->row_count) {
        char next[110];snprintf(next,sizeof(next),"v %s",m->rows[focused+1].title);
        int width=pj_ui_measure(next);if(width>120)width=120;
        pj_ui_text(fb,PJ_FB_W-12-width,107,next,0x707070u,width+1);
    }
    int width=(int)pj_art_width(row->shape),gap=8,pitch=width+gap,top=124;
    unsigned visible=row->shape==PJ_ART_THUMB?3u:4u,column=s->selected-row->first;
    unsigned first=column>=visible?column-(visible-1):0;
    for(unsigned i=first;i<row->count&&i<first+visible+1;i++) {
        int x=12+(int)(i-first)*pitch;
        const pj_library_item *card=&m->items[row->first+i];
        pj_ui_card(fb,x,top,card,row->shape,s->art,i==column);
        if(row->shape==PJ_ART_THUMB) {
            pj_ui_text(fb,x,top+(int)pj_art_height(row->shape)+5,card->title,i==column?PJ_UI_INK:PJ_UI_MUTED,width);
            if(card->subtitle[0])pj_ui_text(fb,x,top+(int)pj_art_height(row->shape)+18,card->subtitle,0x808080u,width);
        }
    }
    int room=pj_ui_home_credit(fb);
    if(row->shape==PJ_ART_THUMB&&s->status&&s->status[0])pj_ui_text(fb,12,PJ_FB_H-15,s->status,0x808080u,room);
}
static inline void pj_ui_library(uint16_t *fb,const pj_ui_state *s) {
    const pj_live_library *m=s->library;
    pj_ui_rect(fb,0,0,PJ_FB_W,PJ_FB_H,PJ_UI_PAGE);
    pj_ui_header_state(fb,s);
    if(!m->count){pj_ui_text(fb,16,110,s->status,PJ_UI_INK,288);return;}
    unsigned shape=m->rows[0].shape,columns=shape==PJ_ART_THUMB?3u:4u;
    int w=(int)pj_art_width(shape),h=(int)pj_art_height(shape),pitch_x=shape==PJ_ART_THUMB?104:76;
    int pitch_y=h+22,top=30,left=shape==PJ_ART_THUMB?8:10;
    unsigned focus_row=s->selected/columns,first_row=focus_row?focus_row-1:0;
    for(unsigned i=first_row*columns;i<m->count;i++) {
        int y=top+(int)(i/columns-first_row)*pitch_y;
        if(y>=PJ_FB_H)break;
        int x=left+(int)(i%columns)*pitch_x;
        pj_ui_card(fb,x,y,&m->items[i],shape,s->art,i==s->selected);
        pj_ui_text(fb,x,y+h+5,m->items[i].title,i==s->selected?PJ_UI_INK:PJ_UI_MUTED,w);
    }
}
/* Word wrap into at most `lines` rows. */
static inline void pj_ui_paragraph(uint16_t *fb,int x,int y,const char *text,unsigned rgb,int width,unsigned lines) {
    unsigned at=0;
    for(unsigned line=0;line<lines&&text[at];line++) {
        char row[128];unsigned used=0,last_space=0;int measured=0;
        while(text[at+used]&&used<sizeof(row)-1) {
            unsigned index=at+used,before=index;
            int glyph=pj_ui_glyph(text,&index);
            if(measured+pj_font_advance[glyph]>width)break;
            measured+=pj_font_advance[glyph];
            unsigned step=index-before;
            if(used+step>=sizeof(row)-1)break;
            memcpy(row+used,text+before,step);
            if(text[before]==' ')last_space=used;
            used+=step;
        }
        if(text[at+used]&&last_space&&line+1<lines)used=last_space;
        row[used]=0;
        if(line+1==lines&&text[at+used])pj_ui_text(fb,x,y,text+at,rgb,width);
        else pj_ui_text(fb,x,y,row,rgb,width+1);
        at+=used;
        while(text[at]==' ')at++;
        y+=13;
    }
}
static inline void pj_ui_button(uint16_t *fb,int x,int y,int w,const char *label,int focused) {
    pj_ui_rect(fb,x,y,w,18,focused?PJ_UI_ACCENT:0x303030u);
    pj_ui_round(fb,x,y,w,18,PJ_UI_PAGE);
    pj_ui_text(fb,x+(w-pj_ui_measure(label))/2,y+4,label,PJ_UI_INK,w);
}
static inline void pj_ui_clock(char *out,size_t size,uint32_t seconds) {
    unsigned h=seconds/3600,m=seconds/60%60,sec=seconds%60;
    if(h)snprintf(out,size,"%u:%02u:%02u",h,m,sec);
    else snprintf(out,size,"%u:%02u",m,sec);
}
static inline void pj_ui_details(uint16_t *fb,const pj_ui_state *s) {
    const pj_live_library *m=s->library;
    const pj_library_item *item=&m->items[s->selected];
    pj_ui_rect(fb,0,0,PJ_FB_W,PJ_FB_H,PJ_UI_PAGE);
    pj_ui_hero(fb,pj_art_find_hero(s->art,item->handle),120,-1);
    pj_ui_header_state(fb,s);
    pj_ui_card(fb,12,34,item,PJ_ART_POSTER,s->art,0);
    int x=88,width=PJ_FB_W-x-12;
    pj_ui_shadow_title(fb,x,34,item->title,width);
    char meta[96];pj_ui_meta(meta,sizeof(meta),item);
    pj_ui_shadow_text(fb,x,60,item->subtitle[0]?item->subtitle:meta,PJ_UI_MUTED,width);
    const pj_details *d=s->details&&s->details->handle==item->handle&&s->details->valid?s->details:NULL;
    if(d&&d->genres[0])pj_ui_shadow_text(fb,x,73,d->genres,PJ_UI_MUTED,width);
    uint64_t duration=item->duration_ticks/10000000u;
    uint64_t position=s->paused?s->position_seconds:item->resume_ticks/10000000u;
    if(s->countdown&&s->countdown[0]) {
        pj_ui_button(fb,x,92,width>176?176:width,s->countdown,1);
        int cancel=pj_ui_measure("B  Cancel");
        if(s->countdown_next&&s->countdown_next[0])pj_ui_text(fb,x,114,s->countdown_next,PJ_UI_INK,width-cancel-10);
        pj_ui_text(fb,x+width-cancel,114,"B  Cancel",PJ_UI_MUTED,cancel+1);
        if(d&&d->overview[0])pj_ui_paragraph(fb,12,138,d->overview,0xd0d0d0u,296,6);
        if(s->status&&s->status[0])pj_ui_text(fb,12,PJ_FB_H-15,s->status,0x808080u,296);
        return;
    }
    if(item->unavailable) {
        pj_ui_text(fb,x,92,"Not on this Pocket",PJ_UI_INK,width);
        pj_ui_text(fb,x,105,s->unavailable_note&&s->unavailable_note[0]?s->unavailable_note:"Add it with the PC sync tool",PJ_UI_MUTED,width);
    } else if(position&&!item->played) {
        pj_ui_button(fb,x,92,84,s->paused?"Continue":"Resume",s->action==0);
        pj_ui_button(fb,x+92,92,84,"From start",s->action==1);
    } else pj_ui_button(fb,x,92,84,"Play",1);
    if(duration&&position) {
        char now[16],total[16],line[40];
        pj_ui_clock(now,sizeof(now),(uint32_t)position);pj_ui_clock(total,sizeof(total),(uint32_t)duration);
        snprintf(line,sizeof(line),"%s / %s",now,total);
        int filled=(int)((uint64_t)width*position/duration);
        pj_ui_rect(fb,x,114,width,3,0x404040u);pj_ui_rect(fb,x,114,filled>width?width:filled,3,PJ_UI_ACCENT);
        pj_ui_text(fb,x,120,line,PJ_UI_MUTED,width);
    }
    if(d&&d->overview[0])pj_ui_paragraph(fb,12,138,d->overview,0xd0d0d0u,296,6);
    if(s->status&&s->status[0])pj_ui_text(fb,12,PJ_FB_H-15,s->status,0x808080u,296);
}
/* The start screen. With a sign it leads with a guide card (the Chromatic search: what to do,
 * what happened); a status (a fault reason) becomes the card's second line. */
static inline void pj_ui_source(uint16_t *fb,const pj_ui_state *s) {
    pj_ui_rect(fb,0,0,PJ_FB_W,PJ_FB_H,PJ_UI_PAGE);
    pj_ui_image(fb,20,12,PJ_BRAND_BANNER_W,PJ_BRAND_BANNER_H,pj_brand_banner_rgb,pj_brand_banner_alpha);
    pj_ui_text(fb,154,25,"for Analogue Pocket",PJ_UI_MUTED,150);
    int y=54;
    if(s->sign&&s->sign[0]) {
        unsigned rgb=pj_ui_tone(s->sign_tone);
        const char *second=s->status&&s->status[0]?s->status:s->hint;
        pj_ui_rect(fb,20,y,280,46,0x1c1c1cu);pj_ui_round(fb,20,y,280,46,PJ_UI_PAGE);
        pj_ui_rect(fb,20,y+3,3,40,rgb);
        pj_ui_dot(fb,32,y+9,rgb);
        pj_ui_text(fb,44,y+6,s->sign,PJ_UI_INK,248);
        if(second&&second[0])pj_ui_paragraph(fb,32,y+20,second,PJ_UI_MUTED,258,2);
        if(s->progress_total) {
            unsigned done=s->progress_done>s->progress_total?s->progress_total:s->progress_done;
            pj_ui_rect(fb,32,y+41,256,2,0x303030u);pj_ui_rect(fb,32,y+41,(int)(256u*done/s->progress_total),2,rgb);
        }
        y+=52;
    } else y+=6;
    unsigned buttons=(s->can_stored!=0)+(s->alternate_label&&s->alternate_label[0])+(s->can_tether!=0)+1;
    int buttons_at=PJ_FB_H-4-(int)buttons*21;
    /* The link numbers lead (they change while the search runs), then the log line, then the
     * start-up checks as far as they fit. Without link lines the order is the checks' own. */
    int linked=s->link[0]&&s->link[0][0];
    for(unsigned i=0;i<3;i++)if(s->link[i]&&s->link[i][0]&&y+11<=buttons_at-2) {
        pj_ui_text(fb,24,y,s->link[i],0xb4b4b4u,272);y+=12;
    }
    for(unsigned n=0;n<5;n++) {
        unsigned i=linked?(n+4u)%5u:n;
        if(s->diagnostics[i]&&s->diagnostics[i][0]&&y+11<=buttons_at-2) {
            pj_ui_text(fb,24,y,s->diagnostics[i],0x8c8c8cu,272);y+=12;
        }
    }
    if((!s->sign||!s->sign[0])&&s->status&&s->status[0]&&y+11<=buttons_at)pj_ui_text(fb,24,y+2,s->status,PJ_UI_INK,272);
    y=buttons_at;
    if(s->can_stored){pj_ui_button(fb,24,y,272,s->offline_label&&s->offline_label[0]?s->offline_label:"A  Library on this Pocket",1);y+=21;}
    if(s->alternate_label&&s->alternate_label[0]){pj_ui_button(fb,24,y,272,s->alternate_label,0);y+=21;}
    if(s->can_tether){pj_ui_button(fb,24,y,272,"X  Connect to the Chromatic now",!s->can_stored);y+=21;}
    pj_ui_button(fb,24,y,272,"Y  About and credits",0);
}
/* About page, opened with Y from the start screen. Each entry is wrapped to the line count
 * given, so the static text below is laid out once and checked in the PC preview. Full
 * licence texts ship on the card in Notices/PocketLive. */
static inline void pj_ui_credits(uint16_t *fb) {
    static const struct {const char *text;unsigned lines,rgb;} entries[]={
        {PJ_UI_CREATED_BY,1,PJ_UI_INK},
        {"Not made or endorsed by Analogue or Jellyfin",1,PJ_UI_INK},
        {"PocketFin: GPL-3.0-or-later with additional terms",1,PJ_UI_MUTED},
        {"openfpgaOS SDK and runtime (c) 2026 ThinkElastic, Apache-2.0; musl libc, MIT",2,PJ_UI_MUTED},
        {"FPGA image also has the Analogue Pocket Framework (Analogue licence) and Intel FPGA IP (Intel licence)",2,PJ_UI_MUTED},
        {"JPEG decoder: core_jpeg by ultraembedded, Apache-2.0",1,PJ_UI_MUTED},
        {"Roboto font (c) 2011 Google, Apache-2.0",1,PJ_UI_MUTED},
        {"Jellyfin logo (c) Jellyfin contributors, CC BY-SA 4.0",1,PJ_UI_MUTED},
    };
    pj_ui_rect(fb,0,0,PJ_FB_W,PJ_FB_H,PJ_UI_PAGE);
    pj_ui_image(fb,20,10,PJ_BRAND_BANNER_W,PJ_BRAND_BANNER_H,pj_brand_banner_rgb,pj_brand_banner_alpha);
    pj_ui_title(fb,160,19,"About",PJ_UI_INK,136);
    pj_ui_rect(fb,24,52,272,1,0x303030u);
    int y=58;
    for(unsigned i=0;i<sizeof(entries)/sizeof(entries[0]);i++) {
        pj_ui_paragraph(fb,24,y,entries[i].text,entries[i].rgb,272,entries[i].lines);
        y+=13*(int)entries[i].lines+(i==1?6:3);
    }
    pj_ui_text(fb,24,PJ_FB_H-15,"Full notices: Notices/PocketLive on the card",0x808080u,200);
    int width=pj_ui_measure("B  Back");
    pj_ui_text(fb,PJ_FB_W-24-width,PJ_FB_H-15,"B  Back",PJ_UI_MUTED,width+1);
}
static inline void pj_ui_draw(uint16_t *fb,const pj_ui_state *s) {
    if(s->screen==PJ_SCREEN_SOURCE)pj_ui_source(fb,s);
    else if(s->screen==PJ_SCREEN_CREDITS)pj_ui_credits(fb);
    else if(s->screen==PJ_SCREEN_DETAILS&&s->library->count)pj_ui_details(fb,s);
    else if(s->screen==PJ_SCREEN_LIBRARY)pj_ui_library(fb,s);
    else pj_ui_home(fb,s);
}
/* Playback strip below the 16:9 picture: never covers the video. 320x30 buffer. */
#define PJ_OSD_H 30u
static inline void pj_ui_osd(uint16_t *strip,const char *title,uint32_t position,uint32_t duration,const char *note) {
    for(unsigned i=0;i<PJ_FB_W*PJ_OSD_H;i++)strip[i]=0;
    char now[16],total[16];pj_ui_clock(now,sizeof(now),position);pj_ui_clock(total,sizeof(total),duration);
    int total_width=pj_ui_measure(total);
    pj_ui_text(strip,8,3,note&&note[0]?note:title,PJ_UI_INK,PJ_FB_W-16);
    pj_ui_text(strip,8,17,now,PJ_UI_MUTED,50);
    pj_ui_text(strip,PJ_FB_W-8-total_width,17,total,PJ_UI_MUTED,total_width+1);
    int left=60,width=PJ_FB_W-left-14-total_width;
    pj_ui_rect(strip,left,21,width,3,0x404040u);
    if(duration) {
        int filled=(int)((uint64_t)width*position/duration);
        pj_ui_rect(strip,left,21,filled>width?width:filled,3,PJ_UI_ACCENT);
    }
}
#endif
