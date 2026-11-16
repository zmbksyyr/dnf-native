/* Original auction algorithms, with independent storage for each group of eight
   refine values. The outer Auction still serializes the REAL refine byte to DB.
   Only these pinned ELFs and verified complete entry instructions are supported. */
#include "run.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN64
typedef int (*Make)(void *,unsigned,unsigned,int,unsigned,void *,unsigned);
typedef int (*Add)(void *,unsigned,unsigned,int,unsigned char *,int *,unsigned,void *,unsigned *,unsigned);
typedef int (*Get)(void *,unsigned,unsigned,void *,unsigned,int *);
typedef void (*Visit)(void *);
typedef int (*Constraint)(void *,const void *);
static Make original_make;
static Add original_add;
static Get original_get;
static Visit original_destroy, original_update;
static Constraint original_constraint;
static unsigned refine_limit = 7;
static void *original_utf8_conversion;
static const char manual_prices[]="UPDATE auction_average_price AS A JOIN auction_manual_average_price AS B ON A.item_id=B.item_id AND A.upgrade=B.upgrade AND A.seperate_upgrade=B.seperate_upgrade SET A.average_price=B.average_price WHERE B.is_apply=0";
static int utf8_name(char *source,char *destination) {
    size_t n=0; while(n<256 && source[n]) ++n;
    if(n==256) return 0;
    /* Original same-charset callback returns true without filling destination.
       Load always consumes destination, so UTF-8 indexes need a real copy. */
    memcpy(destination,source,n+1); return 1;
}
static CRITICAL_SECTION average_lock;
typedef struct AverageBanks {
    void *owner, *bank[4];
    struct AverageBanks *next;
} AverageBanks;
static AverageBanks *banks;

static void *dictionary(void *owner, unsigned refine) {
    if (refine < 8) return owner;
    AverageBanks *p = banks;
    while (p && p->owner != owner) p = p->next;
    if (!p) {
        p = calloc(1,sizeof(*p));
        if (!p) return NULL;
        p->owner = owner; p->next = banks; banks = p;
    }
    unsigned group = refine / 8;
    if (!p->bank[group]) {
        /* Last original map ends at 0x303c; leave its original ABI intact. */
        void *d = calloc(1,0x303c);
        if (!d) return NULL;
        ((Visit)0x080647ae)(d);
        memcpy((char *)d+0x1824,(char *)owner+0x1824,24);
        p->bank[group] = d;
    }
    return p->bank[group];
}
static int make_price(void *d,unsigned item,unsigned upgrade,int price,unsigned count,void *roi,unsigned refine) {
    if (refine > refine_limit) return 32;
    EnterCriticalSection(&average_lock);
    void *selected = dictionary(d,refine);
    int result = selected ? original_make(selected,item,upgrade,price,count,roi,refine%8) : 9;
    LeaveCriticalSection(&average_lock); return result;
}
static int add_price(void *d,unsigned item,unsigned upgrade,int price,unsigned char *created,int *average,unsigned stackable,void *roi,unsigned *count,unsigned refine) {
    if (refine > refine_limit) return 32;
    EnterCriticalSection(&average_lock);
    void *selected = dictionary(d,refine);
    int result = selected ? original_add(selected,item,upgrade,price,created,average,stackable,roi,count,refine%8) : 9;
    LeaveCriticalSection(&average_lock); return result;
}
static int get_price(void *d,unsigned item,unsigned upgrade,void *roi,unsigned refine,int *price) {
    if (refine > refine_limit) return 32;
    EnterCriticalSection(&average_lock);
    void *selected = dictionary(d,refine);
    int result = selected ? original_get(selected,item,upgrade,roi,refine%8,price) : 9;
    LeaveCriticalSection(&average_lock); return result;
}
static int constraint(void *d,const void *value) {
    EnterCriticalSection(&average_lock);
    int result = original_constraint(d,value);
    for (AverageBanks *p=banks;p;p=p->next) if(p->owner==d)
        for(unsigned i=1;i<4;++i) if(p->bank[i]) original_constraint(p->bank[i],value);
    LeaveCriticalSection(&average_lock); return result;
}
static void update(void *d) {
    EnterCriticalSection(&average_lock);
    original_update(d);
    for (AverageBanks *p=banks;p;p=p->next) if(p->owner==d)
        for(unsigned i=1;i<4;++i) if(p->bank[i]) original_update(p->bank[i]);
    LeaveCriticalSection(&average_lock);
}
static void destroy(void *d) {
    EnterCriticalSection(&average_lock);
    AverageBanks **link=&banks;
    while(*link && (*link)->owner!=d) link=&(*link)->next;
    if(*link) {
        AverageBanks *p=*link; *link=p->next;
        for(unsigned i=1;i<4;++i) if(p->bank[i]) { original_destroy(p->bank[i]); free(p->bank[i]); }
        free(p);
    }
    original_destroy(d);
    LeaveCriticalSection(&average_lock);
}
static int matches(const NrElf *e,uintptr_t pc,const unsigned char *bytes,size_t n) {
    for(unsigned i=0;i<e->header->phnum;++i) {
        const NrElfProgram *p=&e->programs[i];
        if(p->type!=1 || pc<p->vaddr || pc-p->vaddr>p->filesz) continue;
        size_t k=pc-p->vaddr;
        if(n>p->filesz-k || p->offset+k>e->file_size || n>e->file_size-p->offset-k) return 0;
        return !memcmp(e->file+p->offset+k,bytes,n) && !memcmp((void *)pc,bytes,n);
    }
    return 0;
}
static int write_patch(const NrElf *e,uintptr_t pc,const void *value,size_t n) {
    DWORD old,ignored;
    fprintf(stderr,"auction patch sha256=%s pc=0x%lx original=",e->sha256,(unsigned long)pc);
    for(size_t i=0;i<n;++i) fprintf(stderr,"%02x",((unsigned char *)pc)[i]);
    fputs(" replacement=",stderr);
    for(size_t i=0;i<n;++i) fprintf(stderr,"%02x",((const unsigned char *)value)[i]);
    fputs(" scope=execution-memory; restart restores original\n",stderr);
    if(!VirtualProtect((void *)pc,n,PAGE_EXECUTE_READWRITE,&old)) return 0;
    memcpy((void *)pc,value,n);
    return VirtualProtect((void *)pc,n,old,&ignored) && FlushInstructionCache(GetCurrentProcess(),(void *)pc,n);
}
typedef struct { uintptr_t pc; const char *bytes; unsigned length; void *replacement; void **original; } Hook;
static int hook(const NrElf *e,const Hook *h) {
    unsigned char *code=VirtualAlloc(NULL,32,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!code) return 0;
    memcpy(code,h->bytes,h->length); code[h->length]=0xe9;
    uint32_t delta=(uint32_t)(h->pc+h->length-(uintptr_t)(code+h->length+5));
    memcpy(code+h->length+1,&delta,4);
    DWORD old;
    if(!VirtualProtect(code,32,PAGE_EXECUTE_READ,&old) || !FlushInstructionCache(GetCurrentProcess(),code,32)) return 0;
    *h->original=code;
    unsigned char jump[16]; memset(jump,0x90,sizeof(jump)); jump[0]=0xe9;
    delta=(uint32_t)((uintptr_t)h->replacement-h->pc-5); memcpy(jump+1,&delta,4);
    return write_patch(e,h->pc,jump,h->length);
}
#endif

int nr_auction_configure(NrRun *run,const char **error) {
    unsigned level=70,refine=7; int requested=0,utf8_index=0,price_keys=0;
    for(int i=0;i<run->environment_count;++i) {
        const char *s=run->environment[i]; unsigned *target=NULL;
        if(!strcmp(s,"DNF_NATIVE_AUCTION_ENCODING=UTF8")) { utf8_index=1; requested=1; continue; }
        if(!strcmp(s,"DNF_NATIVE_AUCTION_PRICE_KEYS=1")) { price_keys=1; requested=1; continue; }
        if(!strncmp(s,"DNF_NATIVE_AUCTION_LEVEL=",25)) { target=&level; s+=25; }
        else if(!strncmp(s,"DNF_NATIVE_AUCTION_REFINE=",26)) { target=&refine; s+=26; }
        if(!target) continue;
        requested=1;
        if(!*s || strspn(s,"0123456789")!=strlen(s) || strlen(s)>3) { *error="invalid auction limit"; return 0; }
        *target=(unsigned)atoi(s);
    }
    NrElf *e=&run->program;
    int supported=!strcmp(e->sha256,"b31eafd5068b450929133365f071fc2d0dedc7a8a5b80f9c81da44ce4008290c") ||
                  !strcmp(e->sha256,"0339aa6cc51f32c4b50fe9e55f3fd8b00d4b495fcad46871c59a5eda345c9256");
    if(!supported) { if(requested) { *error="auction options require a pinned original auction/point ELF"; return 0; } return 1; }
#ifdef _WIN64
    (void)utf8_index;
    (void)price_keys;
    *error="auction compatibility requires the i386 host"; return 0;
#else
    if(e->bias || !level || level>127 || refine>31) { *error="auction limits exceed original protocol/instruction representation"; return 0; }
    Hook hooks[]={
        {0x08063a36,"\x55\x89\xe5\x81\xec\x98\0\0\0",9,make_price,(void **)&original_make},
        {0x08063c7a,"\x55\x89\xe5\x57\x56\x53\x81\xec\xcc\0\0\0",12,add_price,(void **)&original_add},
        {0x080645d0,"\x55\x89\xe5\x83\xec\x48",6,get_price,(void **)&original_get},
        {0x080649e4,"\x55\x89\xe5\x57\x56\x53",6,destroy,(void **)&original_destroy},
        {0x08064bd0,"\x55\x89\xe5\x57\x56\x53",6,update,(void **)&original_update},
        {0x08063c34,"\x55\x89\xe5\x8b\x45\x08",6,constraint,(void **)&original_constraint}
    };
    struct {uintptr_t pc; const char *old; unsigned n,operand;} sites[]={
        {0x0808281c,"\x80\x7d\xfc\x07",4,3},
        {0x080811d5,"\x83\xf8\x46",3,2},
        {0x0808292b,"\x83\xf8\x46",3,2},
        {0x08083471,"\x3c\x46",2,1},
        {0x0808347c,"\x3c\x46",2,1}
    };
    *error="auction original instruction verification failed";
    for(unsigned i=0;i<sizeof(hooks)/sizeof(*hooks);++i)
        if(!matches(e,hooks[i].pc,(const unsigned char *)hooks[i].bytes,hooks[i].length)) return 0;
    for(unsigned i=0;i<sizeof(sites)/sizeof(*sites);++i)
        if(!matches(e,sites[i].pc,(const unsigned char *)sites[i].old,sites[i].n)) return 0;
    if(!matches(e,0x080647ae,(const unsigned char *)"\x55\x89\xe5\x57\x56\x53",6)) return 0;
    const unsigned char charset[]={0xc7,0x44,0x24,0x04,0xb5,0x49,0x15,0x08};
    Hook utf8_hook={0x0806b7f4,"\x55\x89\xe5\x53\x83\xec\x14",7,utf8_name,&original_utf8_conversion};
    if(utf8_index && !matches(e,0x0806b78e,charset,sizeof(charset))) return 0;
    if(utf8_index && !matches(e,utf8_hook.pc,(const unsigned char *)utf8_hook.bytes,utf8_hook.length)) return 0;
    const unsigned char manual_query[]={0xc7,0x44,0x24,0x04,0xec,0x67,0x15,0x08};
    if(price_keys && !matches(e,0x080703cd,manual_query,sizeof(manual_query))) return 0;
    InitializeCriticalSection(&average_lock); refine_limit=refine;
    *error="auction execution-memory patch failed";
    for(unsigned i=0;i<sizeof(hooks)/sizeof(*hooks);++i) if(!hook(e,&hooks[i])) return 0;
    for(unsigned i=0;i<sizeof(sites)/sizeof(*sites);++i) {
        unsigned char code[8]; memcpy(code,sites[i].old,sites[i].n);
        code[sites[i].operand]=(unsigned char)(i ? level : refine);
        if(!write_patch(e,sites[i].pc,code,sites[i].n)) return 0;
    }
    if(utf8_index) {
        unsigned char code[8]; memcpy(code,charset,8); uint32_t utf8_address=0x081549ab;
        memcpy(code+4,&utf8_address,4);
        if(!write_patch(e,0x0806b78e,code,8)) return 0;
        if(!hook(e,&utf8_hook)) return 0;
    }
    if(price_keys) {
        unsigned char code[8]; memcpy(code,manual_query,8); uint32_t address=(uint32_t)(uintptr_t)manual_prices;
        memcpy(code+4,&address,4); if(!write_patch(e,0x080703cd,code,8)) return 0;
    }
    fprintf(stderr,"auction compatibility level=%u refine=%u; original average algorithms; independent eight-refine banks; DB wire refine unchanged\n",level,refine);
    *error=NULL; return 1;
#endif
}
