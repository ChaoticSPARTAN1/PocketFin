/* SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms and permissions: ADDITIONAL-TERMS.md */
#include <stdio.h>
#include <stdlib.h>
#include "live_library.h"
#include "stream_reader.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"native client check failed at %d\n",__LINE__);exit(1);}} while(0)
static pj_live_library library;
static pj_art_download artwork;
static pj_stream_reader stream;
static unsigned accepted,attempts;
static uint8_t input[65536];
static int push_packet(const uint8_t *p,size_t n) {
    CHECK(pj_av_validate(p,p+32,n-32));
    if(++attempts%3==0)return 0;
    accepted++;return 1;
}
static size_t load(const char *name) {
    FILE *f=fopen(name,"rb");CHECK(f);size_t n=fread(input,1,sizeof(input),f);
    CHECK(!ferror(f)&&feof(f));fclose(f);return n;
}
static void json_checks(void) {
    pj_json j;pj_json_token tokens[64];char text[32];uint64_t number;
    const char good[]="{\"nested\":[{},[1,2]],\"n\":18446744073709551615,\"s\":\"a\\u00e9\\ud83d\\ude80\"}";
    CHECK(pj_json_parse(&j,good,strlen(good),tokens,64));
    CHECK(pj_json_u64(&j,pj_json_find(&j,0,"n"),&number)&&number==UINT64_MAX);
    CHECK(pj_json_text(&j,pj_json_find(&j,0,"s"),text,sizeof(text)));
    CHECK(!strcmp(text,"a\xc3\xa9\xf0\x9f\x9a\x80"));
    CHECK(pj_json_text(&j,pj_json_find(&j,0,"s"),text,5));
    CHECK(!strcmp(text,"a\xc3\xa9"));
    const char *bad[]={"[1,]","{\"a\":1,}","01","-","1.","1e+","true false","[","\"\\q\"","{1:2}","\"bad\ntext\""};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)CHECK(!pj_json_parse(&j,bad[i],strlen(bad[i]),tokens,64));
    CHECK(!pj_json_parse(&j,good,strlen(good),tokens,2));
    const char overflow[]="18446744073709551616";
    CHECK(pj_json_parse(&j,overflow,strlen(overflow),tokens,64));
    CHECK(!pj_json_u64(&j,0,&number));
    const char surrogate[]="\"\\ud800\"";
    CHECK(pj_json_parse(&j,surrogate,strlen(surrogate),tokens,64));
    CHECK(!pj_json_text(&j,0,text,sizeof(text)));
    const uint8_t invalid[]={34,0x80,34};
    CHECK(pj_json_parse(&j,invalid,sizeof(invalid),tokens,64));
    CHECK(!pj_json_text(&j,0,text,sizeof(text)));
    uint8_t request[128];size_t n=pj_rpc_encode(request,sizeof(request),2,99,"{}",2);
    CHECK(n==18&&request[5]==0);request[5]=1;
    uint32_t crc=pj_av_crc_part(0xffffffffu,request,12);
    pj_put32(request+12,pj_av_crc_part(crc,request+16,n-16)^0xffffffffu);
    CHECK(pj_rpc_validate(request,n));request[17]^=1;CHECK(!pj_rpc_validate(request,n));
}
int main(int argc,char **argv) {
    if(argc==3&&!strcmp(argv[1],"--home-probe")) {
        size_t n=load(argv[2]);CHECK(pj_rpc_validate(input,n)&&input[4]==PJ_RPC_HOME);
        CHECK(pj_library_load(&library,input+16,n-16,1));
        printf("PASS: actual native parser accepted local server Home: %u rows, %u items. No display metadata output.\n",library.row_count,library.count);
        return library.count?0:1;
    }
    CHECK(argc==4);json_checks();
    size_t n=load(argv[1]);CHECK(pj_rpc_validate(input,n));
    CHECK(pj_library_load(&library,input+16,n-16,1));
    CHECK(library.count==3&&library.row_count==2);
    CHECK(!strcmp(library.items[0].title,"Synthetic Movie"));
    CHECK(library.items[1].folder&&library.items[2].played);
    CHECK(library.items[0].resume_ticks==50000000);
    CHECK(!strcmp(library.items[2].title,"Caf\xc3\xa9 \xf0\x9f\x9a\x80"));
    n=load(argv[2]);size_t offset=0;pj_art_begin(&artwork,1,PJ_ART_POSTER);
    while(offset<n) {
        CHECK(n-offset>=16);size_t count=16+pj_av_u16(input+offset+6);
        CHECK(count<=n-offset&&pj_rpc_validate(input+offset,count));
        CHECK(pj_art_chunk(&artwork,input+offset+16,count-16));offset+=count;
    }
    CHECK(artwork.complete&&artwork.used==PJ_ART_POSTER_W*PJ_ART_POSTER_H*2);
    for(unsigned i=0;i<artwork.used;i++)CHECK(artwork.bytes[i]==(uint8_t)i);
    uint8_t missing[8]={0,0,0,0,64,0,96,0};pj_art_begin(&artwork,9,PJ_ART_POSTER);
    CHECK(pj_art_chunk(&artwork,missing,8)&&artwork.complete&&!artwork.used);
    missing[4]=63;pj_art_begin(&artwork,9,PJ_ART_POSTER);CHECK(!pj_art_chunk(&artwork,missing,8));
    uint8_t wide[8]={0,0,0,0,64,1,120,0};pj_art_begin(&artwork,9,PJ_ART_BACKDROP); /* 320x120 */
    CHECK(pj_art_chunk(&artwork,wide,8)&&artwork.complete);
    n=load(argv[3]);
    for(unsigned fragment=1;fragment<=4096;fragment=fragment<128?fragment*3:fragment*2) {
        pj_stream_reader_init(&stream,3);accepted=attempts=0;size_t at=0;
        while(at<n) {
            size_t count=n-at;if(count>fragment)count=fragment;
            size_t consumed=0;int result=pj_stream_reader_feed(&stream,input+at,count,&consumed,push_packet);
            CHECK(result>=0);at+=consumed;
        }
        while(stream.used){size_t consumed;CHECK(pj_stream_reader_feed(&stream,NULL,0,&consumed,push_packet)>=0);}
        CHECK(pj_stream_reader_eof(&stream)&&accepted==5);
    }
    pj_stream_reader_init(&stream,4);size_t consumed;
    CHECK(pj_stream_reader_feed(&stream,input,n,&consumed,push_packet)<0);
    pj_stream_reader_init(&stream,3);input[n-1]^=1;
    int result=0;offset=0;
    while(offset<n&&result>=0) {
        result=pj_stream_reader_feed(&stream,input+offset,n-offset,&consumed,push_packet);offset+=consumed;
    }
    CHECK(result<0&&!pj_stream_reader_eof(&stream));
    puts("PASS: native Home/UTF-8/art/RPC and fragmented PJAV/backpressure/CRC/session checks; synthetic gateway source, no hardware.");
    return 0;
}
