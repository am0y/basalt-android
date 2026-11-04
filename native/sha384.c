#include "sha384.h"
#include <string.h>
static const uint64_t k[80] = {
0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL};
static uint64_t rotate(uint64_t x, unsigned n) { return x >> n | x << (64 - n); }
static void block(Sha384* s)
{
    uint64_t w[80];
    for (int i=0;i<16;++i) { w[i]=0; for(int j=0;j<8;++j) w[i]=w[i]<<8|s->block[i*8+j]; }
    for (int i=16;i<80;++i) {
        uint64_t a=w[i-15], b=w[i-2];
        w[i]=w[i-16]+(rotate(a,1)^rotate(a,8)^(a>>7))+w[i-7]+(rotate(b,19)^rotate(b,61)^(b>>6));
    }
    uint64_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (int i=0;i<80;++i) {
        uint64_t t1=h+(rotate(e,14)^rotate(e,18)^rotate(e,41))+((e&f)^(~e&g))+k[i]+w[i];
        uint64_t t2=(rotate(a,28)^rotate(a,34)^rotate(a,39))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
void sha384_init(Sha384* s)
{
    *s=(Sha384){.h={0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL}};
}
void sha384_update(Sha384* s,const void* data,size_t length)
{
    const uint8_t* bytes=data;s->bytes+=length;
    while(length) { size_t n=128-s->used;if(n>length)n=length;memcpy(s->block+s->used,bytes,n);
        s->used+=n;bytes+=n;length-=n;if(s->used==128){block(s);s->used=0;} }
}
void sha384_finish(Sha384* s,char output[97])
{
    uint64_t bits=s->bytes*8;uint8_t pad[256]={0x80};
    size_t n=s->used<112?112-s->used:240-s->used;
    sha384_update(s,pad,n);memset(pad,0,16);
    for(int i=0;i<8;++i)pad[15-i]=(uint8_t)(bits>>(i*8));
    sha384_update(s,pad,16);
    const char hex[]="0123456789abcdef";
    for(int i=0;i<48;++i){uint8_t b=s->h[i/8]>>(56-(i%8)*8);output[i*2]=hex[b>>4];output[i*2+1]=hex[b&15];}
    output[96]=0;
}
