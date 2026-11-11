/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md
 * Bounded compressed AV playback shared by native/SDK backends. No credentials,
 * networking or files here. Feed complete PJAV packets from the RPC receiver.
 */
#ifndef PJ_LIVE_PLAYBACK_H
#define PJ_LIVE_PLAYBACK_H
#include <stddef.h>
#include "../shared/av_stream.h"
#include "../shared/ima_audio.h"
/* 2026-11-10 Core Settings "Buffer" (pocket/core_settings.h). The pictures wait COMPRESSED (the JPEG as it came, a
 * few hundred bytes to 2 KB over the link, at most PJ_AV_LIMIT), each in one piece of a byte store, so a long buffer
 * costs bytes, not 32 KB a place as before. PJ_LIVE_VIDEO_QUEUE is still the Short buffer's 32 places; the store has
 * PJ_LIVE_VIDEO_SLOTS places and PJ_LIVE_VIDEO_BYTES bytes (64 pictures of the largest size, or 1,000 of the 2 KB
 * ones). The sound waits decoded (48 kHz stereo, 4 bytes a frame) in a ring of PJ_LIVE_AUDIO_RING frames (10.9 s,
 * 2 MiB); a buffer mode uses at most audio_cap of it (Short: 65,536 frames, 1.37 s, as before). Memory: 2 MiB of
 * pictures + 2 MiB of sound + 8 KiB of places, against 1 MiB + 256 KiB before (the app's static window is 54 MiB). */
#define PJ_LIVE_VIDEO_QUEUE 32u
#define PJ_LIVE_VIDEO_SLOTS 512u
#define PJ_LIVE_VIDEO_BYTES (2u*1024u*1024u)
#define PJ_LIVE_AUDIO_FRAMES 65536u      /* the Short buffer's sound (and the ring before 2026-11-10) */
#define PJ_LIVE_AUDIO_RING 524288u       /* a power of two: the ring's index is masked */
/* 2026-11-10 (first real playback over the Chromatic link: the stream came at ~0.7 of real time, the sound starved
 * 4-5 times a second and the picture stuttered with it). Sound in hand (48 kHz frames, beyond the block held back for
 * the END trim) before playback starts, and again after the sound ran dry: playback then pauses, picture and sound
 * together, until this much has come, instead of playing each scrap as it lands. Both are capped so the pictures that
 * come with that sound still fit the picture queue (4 places spare), never below the 0.25 s start of before. */
#define PJ_LIVE_START_FRAMES 24000u    /* 0.5 s (was 12,000: 0.25 s) */
#define PJ_LIVE_REBUFFER_FRAMES 36000u /* 0.75 s */
#define PJ_LIVE_MIN_FRAMES 12000u
#define PJ_LIVE_STALL_US 3000000u      /* no sound played (or, while waiting, none arrived) this long: failed */
/* The buffer modes. Short is the buffer of before (0.5 s to start, 0.75 s after the sound ran dry, at most 32
 * pictures and 1.37 s of sound waiting); it is also what pj_live_init sets. Normal (the Core Settings default) and
 * Long bank 2 s and 4 s of the title before it plays and again after a refill, and may hold up to 8 s of it, so a
 * busy scene that needs more than the link carries for a while plays out of the bank. Model (Jellymatic's tether
 * simulation of this player, console timing, 384 kHz, 27.1 KB/s, a 24 fps title of 530 B pictures with a 1 s scene of 2.1 KB
 * ones every 9 s): Short 5 refills (3.0 s paused) after a 0.6 s start; Normal none after 1.75 s; Long none after 3.5 s. */
enum {PJ_LIVE_BUFFER_SHORT,PJ_LIVE_BUFFER_LONG,PJ_LIVE_BUFFER_NORMAL,PJ_LIVE_BUFFER_MODES};
#ifndef PJ_LIVE_LONG_SECONDS
#define PJ_LIVE_LONG_SECONDS 4u        /* the Long buffer's start and refill figure (a host model may try others) */
#endif
#define PJ_LIVE_NORMAL_SECONDS 2u
#define PJ_LIVE_LONG_CAP_FRAMES (8u*48000u)
typedef struct {unsigned start_frames,rebuffer_frames,audio_cap,video_cap,gain_shift;} pj_live_options;
static inline pj_live_options pj_live_buffer_options(unsigned mode,unsigned gain_shift) {
    pj_live_options o={PJ_LIVE_START_FRAMES,PJ_LIVE_REBUFFER_FRAMES,PJ_LIVE_AUDIO_FRAMES,PJ_LIVE_VIDEO_QUEUE,0};
    if(mode==PJ_LIVE_BUFFER_LONG||mode==PJ_LIVE_BUFFER_NORMAL) {
        o.start_frames=o.rebuffer_frames=(mode==PJ_LIVE_BUFFER_LONG?PJ_LIVE_LONG_SECONDS:PJ_LIVE_NORMAL_SECONDS)*48000u;
        o.audio_cap=PJ_LIVE_LONG_CAP_FRAMES;o.video_cap=PJ_LIVE_VIDEO_SLOTS;
    }
    o.gain_shift=gain_shift>2u?2u:gain_shift;
    return o;
}
typedef struct {
    void *context;
    void (*audio_reset)(void *);
    unsigned (*audio_free)(void *);
    unsigned (*audio_write)(void *,const int16_t *,unsigned);
    int (*video_begin)(void *,const uint8_t *,size_t,uint16_t *,unsigned,unsigned,uint32_t);
    int (*video_poll)(void *,unsigned,uint32_t);
    void (*video_abort)(void *);
    void (*present)(void *,const uint16_t *,unsigned,unsigned,uint64_t);
} pj_live_ops;
typedef struct {uint32_t size,at;uint64_t pts;} pj_live_picture; /* at: where its bytes start in vbytes */
typedef struct {
    pj_av_receiver receiver;
    pj_live_ops ops;
    pj_live_options opts;
    pj_live_picture video[PJ_LIVE_VIDEO_SLOTS];
    unsigned vhead,vcount,ahead,acount,capacity,hold_frames,factor;
    uint32_t vtail; /* the next free byte of vbytes (pictures are stored in order, wrapping to 0) */
    uint64_t generated,written,clock,raw_clock,end_frames,decoded_pts,origin;
    uint32_t clock_us,last_progress_us;
    uint64_t shown,dropped,audio_starves;
    uint64_t rebuffers,seen_generated;uint32_t arrival_us; /* pauses to refill after the sound ran dry; sound arriving */
    uint64_t clipped; /* samples the volume setting saturated */
    int16_t previous;
    uint8_t configured,ended,started,busy,ready,first_shown,have_previous,starved,failed,complete,have_origin,rebuffering;
    uint8_t store_full; /* a picture found no room while buffering: start (or end the refill) with what is in hand */
    const char *why; /* first failure, for the screen */
    /* Bulk buffers last: pj_live_init clears only what is above (they are written before they are read). */
    uint16_t decoded[256*144];
    int16_t block[2048];
    uint8_t vbytes[PJ_LIVE_VIDEO_BYTES];
    int16_t audio[PJ_LIVE_AUDIO_RING*2];
} pj_live_player;

static inline void pj_live_stop(pj_live_player *p) {
    p->ops.video_abort(p->ops.context);p->ops.audio_reset(p->ops.context);
    p->busy=p->ready=p->started=0;
}
static inline int pj_live_fail(pj_live_player *p,const char *why) {p->failed=1;p->why=why;pj_live_stop(p);return -1;}
/* The options of before (Short buffer, no gain) unless pj_live_set_options says otherwise; kept across a CONFIG. */
static inline void pj_live_init(pj_live_player *p,pj_live_ops ops) {
    memset(p,0,offsetof(pj_live_player,decoded));p->ops=ops;p->opts=pj_live_buffer_options(PJ_LIVE_BUFFER_SHORT,0);
}
/* Any time: a smaller cap only stops new packets until the queue is under it; thresholds count from the next wait. */
static inline void pj_live_set_options(pj_live_player *p,pj_live_options o) {
    if(o.video_cap<4u)o.video_cap=4u;
    if(o.video_cap>PJ_LIVE_VIDEO_SLOTS)o.video_cap=PJ_LIVE_VIDEO_SLOTS;
    if(o.audio_cap<PJ_LIVE_MIN_FRAMES*2u)o.audio_cap=PJ_LIVE_MIN_FRAMES*2u;
    if(o.audio_cap>PJ_LIVE_AUDIO_RING)o.audio_cap=PJ_LIVE_AUDIO_RING;
    if(o.gain_shift>2u)o.gain_shift=2u;
    p->opts=o;
}
static inline void pj_live_sample(pj_live_player *p,int sample) {
    unsigned tail=(p->ahead+p->acount)&(PJ_LIVE_AUDIO_RING-1);
    p->audio[tail*2]=p->audio[tail*2+1]=(int16_t)sample;
    p->acount++;p->generated++;
}
/* Where a picture of `size` bytes goes in vbytes, or ~0u: the bytes from vtail to the oldest waiting picture are
 * free, wrapping at the end. The writer never reaches the oldest picture's first byte (that is the decoder's until it
 * is done), so a full store and an empty one are never confused. */
static inline uint32_t pj_live_vplace(const pj_live_player *p,uint32_t size) {
    if(!p->vcount)return size<=PJ_LIVE_VIDEO_BYTES?0u:~0u;
    uint32_t head=p->video[p->vhead].at;
    if(p->vtail>=head) {
        if(p->vtail+size<=PJ_LIVE_VIDEO_BYTES)return p->vtail;
        return size<head?0u:~0u;
    }
    return p->vtail+size<head?p->vtail:~0u;
}
static inline void pj_live_vpop(pj_live_player *p) {
    p->vhead=(p->vhead+1)%PJ_LIVE_VIDEO_SLOTS;p->vcount--;
    if(!p->vcount)p->vtail=0;
}
/* +6 dB = x2, +12 dB = x4, saturated (never wrapped). */
static inline int pj_live_gain(pj_live_player *p,int value) {
    if(!p->opts.gain_shift)return value;
    int v=value*(1<<p->opts.gain_shift);
    if(v>32767){p->clipped++;return 32767;}
    if(v<-32768){p->clipped++;return -32768;}
    return v;
}
/* 1 accepted/dropped-stale, 0 backpressure (retry SAME packet), -1 fault.
 * On backpressure no receiver/queue state is advanced. Decoder owns the queued
 * encoded buffer until poll completes; caller may reuse input after return 1.
 */
static inline int pj_live_feed(pj_live_player *p,const uint8_t *packet,size_t bytes) {
    /* CRC is checked once, by pj_av_receive below; this only bounds the header. */
    if(p->failed||bytes<PJ_AV_HEADER||!pj_av_header(packet)||
       pj_av_u32(packet+24)!=bytes-PJ_AV_HEADER)return pj_live_fail(p,"bad packet");
    const uint8_t *data=packet+PJ_AV_HEADER;size_t size=bytes-PJ_AV_HEADER;
    unsigned kind=packet[5];uint64_t pts=pj_av_u64(packet+16);
    if(pts>172800ull*90000ull)return pj_live_fail(p,"bad timestamp");
    if(kind==PJ_AV_CONFIG) {
        pj_av_receiver r={0};
        if(pj_av_receive(&r,packet,data,size)!=1||r.video_codec!=1||r.audio_codec!=1||
           (r.rate!=16000&&r.rate!=24000&&r.rate!=48000))return pj_live_fail(p,"unsupported format");
        pj_live_ops ops=p->ops;pj_live_options opts=p->opts;pj_live_stop(p);pj_live_init(p,ops);p->opts=opts;p->receiver=r;
        p->capacity=ops.audio_free(ops.context);
        if(p->capacity<4096)return pj_live_fail(p,"audio ring small");
        p->factor=48000u/r.rate;
        p->hold_frames=(1+2*(r.block-4))*p->factor+480;
        p->configured=1;return 1;
    }
    if(!p->configured)return pj_live_fail(p,"no config");
    if(p->ended)return pj_live_fail(p,"data after end");
    if(packet[6]&PJ_AV_DISCONTINUITY)return pj_live_fail(p,"discontinuity"); /* seek uses new session */
    pj_av_receiver candidate=p->receiver;
    int accepted=pj_av_receive(&candidate,packet,data,size);
    if(accepted<0)return pj_live_fail(p,"packet order/CRC");
    if(!accepted)return 1;
    /* A seek on stored media resumes mid-file with its original timestamps.
     * The first packet becomes time zero, so no gap is mistaken for lost audio. */
    if(!p->have_origin){p->origin=pts;p->have_origin=1;}
    pts=pts>p->origin?pts-p->origin:0;
    if(kind==PJ_AV_VIDEO) {
        int places=p->vcount<p->opts.video_cap;
        uint32_t at=places?pj_live_vplace(p,(uint32_t)size):~0u;
        if(at==~0u) {
            /* Full. Holding the packet back is right only while sound is in hand: the sound
             * clock is the master, and the sound comes down the same stream behind this
             * picture. Before playback has started, or with under 50 ms of sound left, holding
             * would stop the reads that bring the sound (pictures for ever, no sound: a freeze
             * on the first picture). There this newest picture is given up instead and the
             * stream keeps moving; the queued ones keep their times, so sync is unchanged.
             * A long buffer whose BYTES run out (large pictures; the Short buffer's 32 places
             * always fit, PJ_LIVE_VIDEO_BYTES is twice 32 of the largest) starts with what it has. */
            unsigned free_now=p->ops.audio_free(p->ops.context);
            unsigned lead=free_now<p->capacity?p->capacity-free_now:0;
            if(p->started&&!p->rebuffering&&p->acount+lead>p->hold_frames+2400u)return 0; /* refilling: as before start */
            if(places&&p->vcount>=4u)p->store_full=1;
            p->dropped++;p->receiver=candidate;return 1;
        }
        unsigned index=(p->vhead+p->vcount)%PJ_LIVE_VIDEO_SLOTS;
        p->video[index].size=(uint32_t)size;p->video[index].pts=pts;p->video[index].at=at;
        memcpy(p->vbytes+at,data,size);p->vtail=at+(uint32_t)size;p->vcount++;
    } else if(kind==PJ_AV_AUDIO) {
        const unsigned factor=p->factor;
        uint64_t target=(pts*p->receiver.rate+45000)/90000,current=p->generated/factor;
        size_t samples=1+2*(size-4);
        uint64_t gap=target>current?target-current:0,skip=current>target?current-target:0;
        if(gap>p->receiver.rate/2)return pj_live_fail(p,"audio gap");
        size_t needed=(size_t)gap*factor+(skip<samples?(samples-(size_t)skip)*factor:0);
        if(needed>p->opts.audio_cap||p->acount>p->opts.audio_cap-needed)return 0;
        if(pj_ima_decode(data,size,p->block,sizeof(p->block)/sizeof(p->block[0]))!=samples)return pj_live_fail(p,"audio decode");
        if(gap){p->previous=0;p->have_previous=1;}
        while(gap--)for(unsigned phase=0;phase<factor;phase++)pj_live_sample(p,0);
        for(size_t i=(size_t)(skip<samples?skip:samples);i<samples;i++) {
            int value=pj_live_gain(p,p->block[i]),old=p->have_previous?p->previous:value;
            /* Continuous linear interpolation up to 48 kHz; IMA block boundaries
             * do not reset history. Always emits exactly `factor` frames. */
            for(int phase=1;phase<=(int)factor;phase++)pj_live_sample(p,old+(value-old)*phase/(int)factor);
            p->previous=(int16_t)value;p->have_previous=1;
        }
    } else if(kind==PJ_AV_END) {
        uint64_t end=(pts*48000+45000)/90000;
        if(end<p->written||end>p->generated||p->generated-end>p->acount)return pj_live_fail(p,"bad end");
        p->acount-=(unsigned)(p->generated-end);p->generated=end;p->end_frames=end;p->ended=1;
        unsigned fade=p->acount<480?p->acount:480;
        for(unsigned i=0;i<fade;i++) {
            unsigned index=(p->ahead+p->acount-fade+i)&(PJ_LIVE_AUDIO_RING-1);
            int denominator=fade>1?(int)fade-1:1;
            int sample=p->audio[index*2]*(int)(fade-1-i)/denominator;
            p->audio[index*2]=p->audio[index*2+1]=(int16_t)sample;
        }
    }
    p->receiver=candidate;return 1;
}
/* Sound to have in hand before playing (the buffer mode's start or refill figure), capped so that the pictures
 * arriving with it fit the picture queue with 4 places spare (Short: at 60 fps 28 pictures are 0.47 s of the
 * title) and the sound fits its cap. */
static inline unsigned pj_live_target(const pj_live_player *p,unsigned want) {
    unsigned fps=p->receiver.fps?p->receiver.fps:30u;
    unsigned room=(p->opts.video_cap-4u)*48000u/fps;
    room=room>p->hold_frames?room-p->hold_frames:0u;
    unsigned sound=p->opts.audio_cap>p->hold_frames+9600u?p->opts.audio_cap-p->hold_frames-9600u:PJ_LIVE_MIN_FRAMES;
    if(room>sound)room=sound;
    if(want>room)want=room;
    return want<PJ_LIVE_MIN_FRAMES?PJ_LIVE_MIN_FRAMES:want;
}
/* One service tick; backends must not wait on input/decoder readiness. A SDK
 * presentation may take one VSync and must be included in measured loop timing. */
static inline int pj_live_service(pj_live_player *p,uint32_t now_us) {
    if(p->failed)return -1;
    if(!p->configured||p->complete)return p->complete?1:0;
    unsigned free=p->ops.audio_free(p->ops.context);
    if(free>p->capacity)return pj_live_fail(p,"audio ring");
    unsigned queued=p->capacity-free;
    unsigned available=p->acount;
    if(!p->ended)available=available>p->hold_frames?available-p->hold_frames:0;
    int enough=p->store_full&&available>=PJ_LIVE_MIN_FRAMES; /* the pictures fill the store first: play what is here */
    if(!p->started&&(enough||available>=(p->ended?1u:pj_live_target(p,p->opts.start_frames)))) {
        p->started=1;p->clock_us=p->last_progress_us=now_us;p->store_full=0;
    }
    if(p->generated!=p->seen_generated){p->seen_generated=p->generated;p->arrival_us=now_us;}
    if(p->started) {
        if(queued<=8&&!p->ended&&!p->starved&&p->written) {
            p->audio_starves++;p->starved=1;
            if(!p->rebuffering){p->rebuffering=1;p->rebuffers++;p->store_full=0;}
        }
        if(p->rebuffering) {
            /* Refilling: nothing more is played until enough sound waits (or the stream ended); the clock, and with
             * it the picture, stands still meanwhile. Failed only if no sound at all arrives for PJ_LIVE_STALL_US. */
            if(p->ended||enough||available>=pj_live_target(p,p->opts.rebuffer_frames)){p->rebuffering=0;p->store_full=0;p->clock_us=p->last_progress_us=now_us;}
            else {
                if((uint32_t)(now_us-p->arrival_us)>PJ_LIVE_STALL_US)return pj_live_fail(p,"audio stalled 3s");
                available=0;p->last_progress_us=now_us;
            }
        }
        if(queued>2400)p->starved=0;
        // Limit DAC lead to 0.5 seconds, leaving the tail available to trim.
        unsigned room=queued<24000?24000-queued:0;if(room>free)room=free;
        if(available>room)available=room;
        unsigned contiguous=PJ_LIVE_AUDIO_RING-p->ahead;if(available>contiguous)available=contiguous;
        if(available) {
            unsigned n=p->ops.audio_write(p->ops.context,p->audio+p->ahead*2,available);
            if(n>available)return pj_live_fail(p,"audio write");
            p->ahead=(p->ahead+n)&(PJ_LIVE_AUDIO_RING-1);p->acount-=n;p->written+=n;
        }
        queued=p->capacity-p->ops.audio_free(p->ops.context);
        uint64_t raw=p->written>queued?p->written-queued:0;
        if(raw!=p->raw_clock){p->raw_clock=raw;p->clock_us=p->last_progress_us=now_us;}
        uint64_t predicted=raw+(uint64_t)(uint32_t)(now_us-p->clock_us)*48000/1000000;
        if(predicted>p->written)predicted=p->written;
        if(predicted>p->clock)p->clock=predicted;
        if(!p->ended&&(uint32_t)(now_us-p->last_progress_us)>PJ_LIVE_STALL_US)return pj_live_fail(p,"audio stalled 3s");
    }
    uint64_t clock_pts=p->clock*90000/48000;
    /* Budget must clear a whole frame between audio refills: a small value here
     * silently caps the frame rate however fast the decoder is. Two services per pass
     * (2 x 8192 pixel reads) cut a 256x144 frame from 5 passes to 3, so the decoder
     * waits less on the CPU between them. */
    for(unsigned service=0;service<2&&p->busy;service++) {
        int result=p->ops.video_poll(p->ops.context,8192,now_us);
        if(result<0)return pj_live_fail(p,"picture decode");
        if(result>0){p->busy=0;p->ready=1;pj_live_vpop(p);}
    }
    if(p->ready&&(!p->first_shown||(p->started&&p->decoded_pts<=clock_pts))) {
        p->ops.present(p->ops.context,p->decoded,p->receiver.width,p->receiver.height,p->decoded_pts);
        p->shown++;p->first_shown=1;p->ready=0;
    }
    if(!p->busy&&!p->ready&&p->vcount) {
        pj_live_picture *v=&p->video[p->vhead];
        uint64_t frame_ticks=90000/p->receiver.fps;
        if(p->first_shown&&p->started&&v->pts+frame_ticks<clock_pts) {
            p->dropped++;pj_live_vpop(p);
        } else {
            if(!p->ops.video_begin(p->ops.context,p->vbytes+v->at,v->size,p->decoded,p->receiver.width,p->receiver.height,now_us))return pj_live_fail(p,"picture refused");
            p->decoded_pts=v->pts;p->busy=1;
        }
    }
    if(p->ended&&p->clock>=p->end_frames&&!p->acount&&!p->vcount&&!p->busy&&!p->ready) {
        p->complete=1;return 1;
    }
    return 0;
}
/* Seconds of the title waiting (sound in hand, plus what the DAC held at the last service), in tenths. No call out. */
static inline unsigned pj_live_buffered_tenths(const pj_live_player *p) {
    if(!p->configured)return 0;
    uint64_t queued=p->written>p->raw_clock?p->written-p->raw_clock:0;
    return (unsigned)(((uint64_t)p->acount+queued)/4800u);
}
#endif
