#include "crypto.h"
#include <string.h>
#include <windows.h>
#include <wincrypt.h>

/* ======================================================================
 * MD5 (RFC 1321)
 * ====================================================================== */
typedef struct {
    uint32_t a, b, c, d;
    uint64_t total;
    uint8_t  buf[64];
    size_t   used;
} md5_ctx;

static const uint32_t MD5_K[64] = {
    0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
    0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
    0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
    0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
    0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
    0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
    0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
    0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u
};
static const int MD5_S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};
#define ROTL(x,n) (((x)<<(n))|((x)>>(32-(n))))

static void md5_transform(md5_ctx *c, const uint8_t block[64]) {
    uint32_t m[16];
    int i;
    for (i = 0; i < 16; i++)
        m[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1]<<8) |
               ((uint32_t)block[i*4+2]<<16) | ((uint32_t)block[i*4+3]<<24);
    uint32_t a=c->a,b=c->b,cc=c->c,d=c->d;
    for (i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16)      { f = (b & cc) | (~b & d);            g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc);            g = (5*i+1)%16; }
        else if (i < 48) { f = b ^ cc ^ d;                     g = (3*i+5)%16; }
        else             { f = cc ^ (b | ~d);                  g = (7*i)%16; }
        uint32_t tmp = d; d = cc; cc = b;
        b = b + ROTL(a + f + MD5_K[i] + m[g], MD5_S[i]);
        a = tmp;
    }
    c->a+=a; c->b+=b; c->c+=cc; c->d+=d;
}
static void md5_init(md5_ctx *c) {
    c->a=0x67452301u; c->b=0xefcdab89u; c->c=0x98badcfeu; c->d=0x10325476u;
    c->total=0; c->used=0;
}
static void md5_update(md5_ctx *c, const uint8_t *p, size_t n) {
    c->total += (uint64_t)n;
    while (n) {
        size_t take = 64 - c->used;
        if (take > n) take = n;
        memcpy(c->buf + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used == 64) { md5_transform(c, c->buf); c->used = 0; }
    }
}
static void md5_final(md5_ctx *c, uint8_t out[16]) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    md5_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->used != 56) md5_update(c, &zero, 1);
    uint8_t tail[8];
    for (int i=0;i<8;i++) tail[i] = (uint8_t)(bits >> (8*i));
    md5_update(c, tail, 8);
    uint32_t words[4] = { c->a, c->b, c->c, c->d };
    for (int w=0; w<4; w++)
        for (int i=0;i<4;i++)
            out[w*4+i] = (uint8_t)(words[w] >> (8*i));
}
void md5_hash(const uint8_t *data, size_t len, uint8_t out[16]) {
    md5_ctx c; md5_init(&c); md5_update(&c, data, len); md5_final(&c, out);
}
void md5_hex(const uint8_t *data, size_t len, char out_hex[33]) {
    static const char *H = "0123456789abcdef";
    uint8_t d[16]; md5_hash(data, len, d);
    for (int i=0;i<16;i++) { out_hex[i*2]=H[d[i]>>4]; out_hex[i*2+1]=H[d[i]&15]; }
    out_hex[32] = '\0';
}

/* ======================================================================
 * SHA-1 (RFC 3174)
 * ====================================================================== */
typedef struct {
    uint32_t h[5];
    uint64_t total;
    uint8_t  buf[64];
    size_t   used;
} sha1_ctx;

static void sha1_transform(sha1_ctx *c, const uint8_t block[64]) {
    uint32_t w[80];
    for (int i=0;i<16;i++)
        w[i] = ((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|
               ((uint32_t)block[i*4+2]<<8)|((uint32_t)block[i*4+3]);
    for (int i=16;i<80;i++) w[i] = ROTL(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4];
    for (int i=0;i<80;i++) {
        uint32_t f,k;
        if (i<20)      { f=(b&cc)|(~b&d); k=0x5a827999u; }
        else if (i<40) { f=b^cc^d;        k=0x6ed9eba1u; }
        else if (i<60) { f=(b&cc)|(b&d)|(cc&d); k=0x8f1bbcdcu; }
        else           { f=b^cc^d;        k=0xca62c1d6u; }
        uint32_t t = ROTL(a,5)+f+e+k+w[i];
        e=d; d=cc; cc=ROTL(b,30); b=a; a=t;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d; c->h[4]+=e;
}
static void sha1_init(sha1_ctx *c) {
    c->h[0]=0x67452301u; c->h[1]=0xefcdab89u; c->h[2]=0x98badcfeu;
    c->h[3]=0x10325476u; c->h[4]=0xc3d2e1f0u; c->total=0; c->used=0;
}
static void sha1_update(sha1_ctx *c, const uint8_t *p, size_t n) {
    c->total += (uint64_t)n;
    while (n) {
        size_t take = 64 - c->used; if (take>n) take=n;
        memcpy(c->buf+c->used, p, take); c->used+=take; p+=take; n-=take;
        if (c->used==64) { sha1_transform(c,c->buf); c->used=0; }
    }
}
static void sha1_final(sha1_ctx *c, uint8_t out[20]) {
    uint64_t bits = c->total*8;
    uint8_t pad=0x80; sha1_update(c,&pad,1);
    uint8_t zero=0; while (c->used!=56) sha1_update(c,&zero,1);
    uint8_t tail[8];
    for (int i=0;i<8;i++) tail[i]=(uint8_t)(bits>>(56-8*i));
    sha1_update(c,tail,8);
    for (int i=0;i<5;i++)
        for (int j=0;j<4;j++) out[i*4+j]=(uint8_t)(c->h[i]>>(24-8*j));
}
void sha1_hash(const uint8_t *data, size_t len, uint8_t out[20]) {
    sha1_ctx c; sha1_init(&c); sha1_update(&c,data,len); sha1_final(&c,out);
}

/* ======================================================================
 * Base64
 * ====================================================================== */
static const char B64[] =
 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
size_t base64_encode(const uint8_t *in, size_t len, char *out) {
    size_t i=0, o=0;
    while (i+3 <= len) {
        uint32_t v = ((uint32_t)in[i]<<16)|((uint32_t)in[i+1]<<8)|in[i+2];
        out[o++]=B64[(v>>18)&63]; out[o++]=B64[(v>>12)&63];
        out[o++]=B64[(v>>6)&63];  out[o++]=B64[v&63];
        i+=3;
    }
    if (i<len) {
        uint32_t v = (uint32_t)in[i]<<16;
        int rem = (int)(len-i);
        if (rem==2) v |= (uint32_t)in[i+1]<<8;
        out[o++]=B64[(v>>18)&63]; out[o++]=B64[(v>>12)&63];
        out[o++]= rem==2 ? B64[(v>>6)&63] : '=';
        out[o++]='=';
    }
    out[o]='\0';
    return o+1;
}
static int b64val(int ch) {
    if (ch>='A'&&ch<='Z') return ch-'A';
    if (ch>='a'&&ch<='z') return ch-'a'+26;
    if (ch>='0'&&ch<='9') return ch-'0'+52;
    if (ch=='+') return 62;
    if (ch=='/') return 63;
    return -1;
}
size_t base64_decode(const char *in, size_t len, uint8_t *out) {
    int quad[4]; int qn=0; size_t o=0;
    for (size_t i=0;i<len;i++) {
        int ch = (unsigned char)in[i];
        if (ch=='\r'||ch=='\n'||ch==' '||ch=='\t') continue;
        if (ch=='=') { quad[qn++]= -2; }
        else { int v=b64val(ch); if (v<0) return (size_t)-1; quad[qn++]=v; }
        if (qn==4) {
            if (quad[0]<0||quad[1]<0) return (size_t)-1;
            out[o++] = (uint8_t)((quad[0]<<2)|(quad[1]>>4));
            if (quad[2]>=0) out[o++] = (uint8_t)((quad[1]<<4)|(quad[2]>>2));
            if (quad[3]>=0) out[o++] = (uint8_t)((quad[2]<<6)|quad[3]);
            qn=0;
        }
    }
    if (qn!=0) return (size_t)-1;
    return o;
}

/* ======================================================================
 * Big integer helpers (little-endian 32-bit limbs)
 * ====================================================================== */
#define BN_LIMBS 80
typedef struct { int n; uint32_t v[BN_LIMBS]; } bn;

static void bn_zero(bn *a){ a->n=0; memset(a->v,0,sizeof(a->v)); }
static void bn_set_u32(bn *a, uint32_t x){ bn_zero(a); a->v[0]=x; a->n = x?1:0; }
static void bn_from_limbs(bn *a, const uint32_t *limbs, int cnt){
    bn_zero(a);
    if (cnt>BN_LIMBS) cnt=BN_LIMBS;
    memcpy(a->v,limbs,(size_t)cnt*4); a->n=cnt;
    while (a->n>0 && a->v[a->n-1]==0) a->n--;
}
static void bn_from_be(bn *a, const uint8_t *p, int len) {
    bn_zero(a);
    while (len>0 && *p==0) { p++; len--; }
    int limbs = (len+3)/4;
    if (limbs>BN_LIMBS) limbs=BN_LIMBS;
    for (int i=0;i<limbs;i++) {
        int idx = len-1-i*4;
        uint32_t val=0;
        for (int j=0;j<4;j++) {
            if (idx-j>=0) val |= (uint32_t)p[idx-j] << (8*j);
        }
        a->v[i]=val;
    }
    a->n=limbs;
    while (a->n>0 && a->v[a->n-1]==0) a->n--;
}
static void bn_to_be_fixed(const bn *a, uint8_t *out, int k) {
    memset(out,0,(size_t)k);
    for (int i=0;i<a->n && i<k/4+1;i++) {
        for (int j=0;j<4;j++) {
            int idx = k-1-i*4-j;
            if (idx>=0) out[idx]=(uint8_t)(a->v[i]>>(8*j));
        }
    }
}
static int bn_cmp_gen(const uint32_t *a, int an, const uint32_t *b, int bn_);
static int bn_cmp(const bn *a, const bn *b) {
    return bn_cmp_gen(a->v,a->n,b->v,b->n);
}
static int bn_cmp_gen(const uint32_t *a, int an, const uint32_t *b, int bn_) {
    int n = an>bn_ ? an : bn_;
    for (int i=n-1;i>=0;i--) {
        uint32_t x = i<an?a[i]:0, y = i<bn_?b[i]:0;
        if (x>y) return 1; if (x<y) return -1;
    }
    return 0;
}
/* a -= b, assumes a>=b */
static void bn_sub(bn *a, const bn *b) {
    int64_t borrow=0;
    for (int i=0;i<a->n;i++) {
        int64_t bv = i<b->n?b->v[i]:0;
        int64_t cur = (int64_t)a->v[i] - bv - borrow;
        if (cur<0) { cur += 0x100000000LL; borrow=1; } else borrow=0;
        a->v[i]=(uint32_t)cur;
    }
    while (a->n>0 && a->v[a->n-1]==0) a->n--;
}
/* Compute n0inv = -n[0]^-1 mod 2^32 (Newton iteration) */
static uint32_t bn_inv32(uint32_t a) {
    uint32_t x = 1;
    for (int i=0;i<5;i++) x = x * (2u - a*x);
    return x;
}

/* Montgomery multiplication: result = a*b*R^{-1} mod n.
   Uses a scratch buffer of 2*s+2 limbs. */
static void mont_mul(const bn *a, const bn *b, const RsaKey *key, bn *result) {
    int s = key->limbs;
    uint32_t t[2*BN_LIMBS+4];
    memset(t,0,sizeof(t));
    /* t = a*b */
    for (int i=0;i<s;i++) {
        uint64_t carry=0;
        for (int j=0;j<s;j++) {
            uint64_t cur = (uint64_t)t[i+j] + (uint64_t)a->v[i]*b->v[j] + carry;
            t[i+j]=(uint32_t)cur; carry=cur>>32;
        }
        int idx=i+s;
        while (carry) { uint64_t cur=(uint64_t)t[idx]+carry; t[idx]=(uint32_t)cur; carry=cur>>32; idx++; }
    }
    /* reduction */
    for (int i=0;i<s;i++) {
        uint32_t m = (uint32_t)((uint64_t)t[i]*key->n0inv);
        uint64_t carry=0;
        for (int j=0;j<s;j++) {
            uint64_t cur = (uint64_t)t[i+j] + (uint64_t)m*key->n[j] + carry;
            t[i+j]=(uint32_t)cur; carry=cur>>32;
        }
        int idx=i+s;
        while (carry) { uint64_t cur=(uint64_t)t[idx]+carry; t[idx]=(uint32_t)cur; carry=cur>>32; idx++; }
    }
    /* result = t / R = limbs t[s .. 2s] (s+1 limbs; the extra top limb may
       be non-zero when the reduction result reaches/exceeds R). */
    bn_zero(result);
    int rlen = s+1;
    for (int i=0;i<rlen;i++) result->v[i]=t[s+i];
    result->n=rlen;
    while (result->n>0 && result->v[result->n-1]==0) result->n--;
    bn nn; nn.n=s; memcpy(nn.v,key->n,sizeof(uint32_t)*(size_t)s);
    /* Reduce until result < n (at most a couple of subtractions). */
    while (bn_cmp(result,&nn)>=0) bn_sub(result,&nn);
}

/* R^2 mod n = 2^(64*s) mod n.
   Keep x < n at all times and repeatedly double: x = 2x, and since 2x < 2n,
   a single conditional subtraction of n brings it back below n. When the top
   carry is set, 2x overflows s limbs; subtracting n (which is < 2^(32s))
   folds it back correctly because 2x - n < n. */
static void compute_r2(const bn *n, int s, uint32_t r2[80]) {
    uint32_t x[BN_LIMBS];
    memset(x,0,sizeof(x));
    x[0]=1;
    for (int i=0;i<64*s;i++) {
        uint32_t carry=0;
        for (int j=0;j<s;j++){
            uint32_t c = x[j]>>31;
            x[j] = (x[j]<<1)|carry;
            carry = c;
        }
        int ge = carry || (bn_cmp_gen(x,s,n->v,s)>=0);
        if (ge) {
            uint64_t borrow=0;
            for (int j=0;j<s;j++){
                uint64_t cur = (uint64_t)x[j] - (uint64_t)n->v[j] - borrow;
                x[j] = (uint32_t)cur;
                borrow = (cur>>63)&1;
            }
            /* borrow may be 1 when carry was set; that's expected and x is
               already the correct residue below n. */
        }
    }
    memset(r2,0,80*4);
    memcpy(r2,x,(size_t)s*4);
}

/* ======================================================================
 * DER / PEM parsing for PKCS#1 RSAPrivateKey
 * ====================================================================== */
typedef struct { const uint8_t *p; const uint8_t *end; } der;

static int der_tlv(der *d, uint8_t want_tag, const uint8_t **val, size_t *len) {
    if (d->p>=d->end) return -1;
    uint8_t tag=*d->p++;
    if (want_tag && tag!=want_tag) return -1;
    if (d->p>=d->end) return -1;
    uint8_t l=*d->p++;
    size_t vlen;
    if (l & 0x80) {
        int nb = l & 0x7f;
        if (nb<=0 || nb>4 || d->p+nb>d->end) return -1;
        vlen=0;
        for (int i=0;i<nb;i++) vlen=(vlen<<8)|*d->p++;
    } else vlen=l;
    if ((size_t)(d->end-d->p)<vlen) return -1;
    *val=d->p; *len=vlen; d->p+=vlen;
    return 0;
}

int rsa_key_from_pem(const char *pem, size_t len, RsaKey *key) {
    static const char *BEG="-----BEGIN RSA PRIVATE KEY-----";
    static const char *END="-----END RSA PRIVATE KEY-----";
    memset(key,0,sizeof(*key));

    /* locate PEM body */
    const char *s=NULL,*e=NULL;
    for (size_t i=0;i+strlen(BEG)<=len;i++) {
        if (!memcmp(pem+i,BEG,strlen(BEG))) { s=pem+i+strlen(BEG); break; }
    }
    if (!s) return -1;
    for (const char *q=s; q+strlen(END)<=pem+len; q++) {
        if (!memcmp(q,END,strlen(END))) { e=q; break; }
    }
    if (!e || e<=s) return -1;

    /* base64_decode has no capacity argument; bound its maximum output first. */
    size_t encoded = 0;
    for (const char *q = s; q < e; ++q)
        if (*q != '\r' && *q != '\n' && *q != ' ' && *q != '\t') ++encoded;
    uint8_t derbuf[2048];
    if (encoded / 4 * 3 > sizeof(derbuf)) return -1;
    size_t derlen = base64_decode(s,(size_t)(e-s),derbuf);
    if (derlen==(size_t)-1 || derlen>sizeof(derbuf)) return -1;

    der d = { derbuf, derbuf+derlen };
    const uint8_t *seq; size_t seqlen;
    if (der_tlv(&d,0x30,&seq,&seqlen)!=0) return -1;
    der inner = { seq, seq+seqlen };

    const uint8_t *v; size_t vl;
    if (der_tlv(&inner,0x02,&v,&vl)!=0) return -1;          /* version */
    if (der_tlv(&inner,0x02,&v,&vl)!=0) return -1;          /* modulus n */
    if (!vl || vl > 317 || !(v[vl-1] & 1)) return -1;
    bn nbn; bn_from_be(&nbn,(const uint8_t*)v,(int)vl);
    if (der_tlv(&inner,0x02,&v,&vl)!=0) return -1;          /* public exponent */
    if (der_tlv(&inner,0x02,&v,&vl)!=0) return -1;          /* private exponent d */
    if (!vl || vl > 317) return -1;
    bn dbn; bn_from_be(&dbn,(const uint8_t*)v,(int)vl);

    /* Determine k from top limb bit length (DER INTEGER may have a leading 0). */
    uint32_t top = nbn.n>0 ? nbn.v[nbn.n-1] : 0;
    int bits=0; while (top) { bits++; top>>=1; }
    key->k = ((nbn.n-1)*32 + bits + 7)/8;
    key->limbs = (key->k+3)/4;
    int limbs = key->limbs;
    if (limbs<=0 || limbs>=80) return -1;
    memcpy(key->n,nbn.v,sizeof(uint32_t)*(size_t)limbs);
    memcpy(key->d,dbn.v,sizeof(uint32_t)*(size_t)limbs);
    /* Montgomery needs -n[0]^-1 mod 2^32 */
    key->n0inv = (uint32_t)(0u - bn_inv32(key->n[0]));

    compute_r2(&nbn,limbs,key->r2);
    return 0;
}

int rsa_public_key_pem(const char *pem, size_t len, char *out, size_t cap) {
    HCRYPTPROV provider = 0;
    HCRYPTKEY key = 0;
    BYTE *decoded = NULL, *blob = NULL, *encoded = NULL;
    CERT_PUBLIC_KEY_INFO *info = NULL;
    DWORD decoded_len = 0, blob_len = 0, info_len = 0, encoded_len = 0;
    int result = -1;
    if (len > 1024 * 1024 ||
        !CryptStringToBinaryA(pem, (DWORD)len, CRYPT_STRING_BASE64HEADER,
                             NULL, &decoded_len, NULL, NULL)) goto done;
    decoded = (BYTE*)LocalAlloc(LMEM_FIXED, decoded_len);
    if (!decoded || !CryptStringToBinaryA(pem, (DWORD)len, CRYPT_STRING_BASE64HEADER,
                                         decoded, &decoded_len, NULL, NULL) ||
        !CryptDecodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, PKCS_RSA_PRIVATE_KEY,
                             decoded, decoded_len, CRYPT_DECODE_ALLOC_FLAG, NULL,
                             &blob, &blob_len) ||
        !CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) ||
        !CryptImportKey(provider, blob, blob_len, 0, 0, &key) ||
        !CryptExportPublicKeyInfo(provider, AT_KEYEXCHANGE, X509_ASN_ENCODING, NULL, &info_len)) goto done;
    info = (CERT_PUBLIC_KEY_INFO*)LocalAlloc(LMEM_FIXED, info_len);
    if (!info || !CryptExportPublicKeyInfo(provider, AT_KEYEXCHANGE, X509_ASN_ENCODING, info, &info_len) ||
        !CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, info,
                             CRYPT_ENCODE_ALLOC_FLAG, NULL, &encoded, &encoded_len)) goto done;
    /* The login implementation supports keys smaller than 80 32-bit limbs. */
    if (encoded_len > 400 || cap < 4 * ((encoded_len + 2) / 3) + 100) goto done;
    char b64[540];
    base64_encode(encoded, encoded_len, b64);
    size_t used = 0;
    const char *begin = "-----BEGIN PUBLIC KEY-----\n";
    memcpy(out, begin, strlen(begin)); used = strlen(begin);
    for (size_t i = 0; b64[i]; i += 64) {
        size_t count = strlen(b64 + i); if (count > 64) count = 64;
        memcpy(out + used, b64 + i, count); used += count; out[used++] = '\n';
        if (count < 64) break;
    }
    strcpy(out + used, "-----END PUBLIC KEY-----\n");
    result = 0;
done:
    if (key) CryptDestroyKey(key);
    if (provider) CryptReleaseContext(provider, 0);
    LocalFree(info); LocalFree(encoded); LocalFree(blob); LocalFree(decoded);
    return result;
}

int rsa_private_encrypt(const RsaKey *key, const uint8_t *data, size_t data_len,
                        uint8_t *out) {
    int k = key->k;
    if (data_len > (size_t)(k-11)) return -1;

    /* em = 0x00 01 FF..FF 00 data  (PKCS#1 v1.5 block type 1) */
    uint8_t em[512];
    if (k>(int)sizeof(em)) return -1;
    memset(em,0xff,(size_t)k);
    em[0]=0x00; em[1]=0x01;
    int pad = k-(int)data_len-3;
    em[2+pad]=0x00;
    memcpy(em+3+pad,data,data_len);

    bn c; bn_from_be(&c,em,k);

    /* Convert c to Montgomery domain: cM = c*r2*R^{-1} = c*R mod n */
    bn cr2; bn_from_limbs(&cr2,key->r2,key->limbs);
    bn cM; mont_mul(&c,&cr2,key,&cM);

    /* oneMont = 1*R mod n = mont_mul(1, r2) */
    bn one; bn_set_u32(&one,1);
    bn oneM; mont_mul(&one,&cr2,key,&oneM);

    /* modular exponentiation m = c^d mod n (Montgomery) */
    bn res = oneM;
    int top=-1;
    for (int i=key->limbs*32-1;i>=0;i--) {
        if ((key->d[i>>5]>>(i&31))&1) { top=i; break; }
    }
    if (top<0) return -1;
    for (int i=top;i>=0;i--) {
        bn sq; mont_mul(&res,&res,key,&sq); res=sq;
        if ((key->d[i>>5]>>(i&31))&1) {
            bn mul; mont_mul(&res,&cM,key,&mul); res=mul;
        }
    }
    /* back to normal domain: m = res*1*R^{-1} */
    bn oneN; bn_set_u32(&oneN,1);
    bn m; mont_mul(&res,&oneN,key,&m);

    uint8_t tmp[512]; bn_to_be_fixed(&m,tmp,k);
    /* Go's big.Int.Bytes(): strip leading zero bytes */
    int off=0; while (off<k && tmp[off]==0) off++;
    int outlen = k-off;
    if (outlen==0) { out[0]=0; return 1; }
    memcpy(out,tmp+off,(size_t)outlen);
    return outlen;
}
