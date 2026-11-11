/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md */
#ifndef PJ_IMA_AUDIO_H
#define PJ_IMA_AUDIO_H
/* Standard mono 4-bit IMA-WAV blocks. Each block has its own predictor/index,
 * so a dropped packet never poisons the next block. Input is untrusted. */
#include <stdint.h>
#include <stddef.h>
static inline size_t pj_ima_decode(const uint8_t *data,size_t size,int16_t *out,size_t capacity) {
    static const int steps[89]={
        7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,
        80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,
        494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
        2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,
        8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
        27086,29794,32767};
    static const int changes[8]={-1,-1,-1,-1,2,4,6,8};
    if(!data||!out||size<4||size>8192||capacity<1+2*(size-4)||data[2]>88||data[3])return 0;
    int predictor=(int)data[0]+((int)data[1]<<8);
    if(predictor>=32768)predictor-=65536;
    int index=data[2];size_t count=0;
    out[count++]=(int16_t)predictor;
    for(size_t i=4;i<size;i++)for(unsigned half=0;half<2;half++) {
        unsigned nibble=(data[i]>>(half*4))&15u;
        int step=steps[index],diff=step>>3;
        if(nibble&1u)diff+=step>>2;
        if(nibble&2u)diff+=step>>1;
        if(nibble&4u)diff+=step;
        predictor+=(nibble&8u)?-diff:diff;
        if(predictor>32767)predictor=32767;
        if(predictor< -32768)predictor=-32768;
        index+=changes[nibble&7u];
        if(index<0)index=0;
        if(index>88)index=88;
        out[count++]=(int16_t)predictor;
    }
    return count;
}
#endif
