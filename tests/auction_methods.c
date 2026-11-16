/* Execute the original two ELF implementations after their actual loader/ctors.
   Private harness only: no production service, DB, or client acceptance claim. */
#include "run.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static NrRun *active;
static uintptr_t main_pc;
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"MARKET_METHOD failure line=%d\n",__LINE__); ExitProcess(112); } } while(0)
typedef struct { uint32_t name,value,size; unsigned char info,other; uint16_t section; } Sym;
static uintptr_t sym(const char *name) {
    NrElf *e=&active->program; NrElfSection *s=(void *)(e->file+e->header->shoff);
    for(unsigned i=0;i<e->header->shnum;++i) if(s[i].type==2) {
        const Sym *v=(void *)(e->file+s[i].offset); const char *ns=(void *)(e->file+s[s[i].link].offset);
        for(unsigned j=0;j<s[i].size/sizeof(*v);++j) if(!strcmp(ns+v[j].name,name)) return e->bias+v[j].value;
    }
    CHECK(0); return 0;
}
static int fixture_main(int argc,char **argv) {
    (void)argc;(void)argv;
    int (*range)(void *,unsigned,unsigned)=(void *)0x08082826;
    CHECK(range(NULL,0,10)==1 && range(NULL,0,11)==0 && range(NULL,10,9)==0);
    unsigned char search[0x200]={0},owner[0x6000]={0},meta[0x30]={0},op[0x40]={0};
    ((void (*)(void *))0x0807e450)(search); *(void **)(search+4)=owner;
    *(unsigned *)(meta+0x14)=100; *(unsigned short *)(meta+0x20)=11105;
    int (*param)(void *,void *,unsigned,unsigned,unsigned long long,void *,int,int,int,unsigned)=(void *)0x080810f6;
    CHECK(param(search,op,16400,0,1,meta,0,0,1,10)==0);
    CHECK(param(search,op,16400,0,1,meta,0,0,1,11)==51);
    *(unsigned *)(meta+0x14)=101; CHECK(param(search,op,16400,0,1,meta,0,0,1,10)==27);
    unsigned char avg[0x303c]={0}; ((void (*)(void *))0x080647ae)(avg);
    unsigned roi[6]={0};
    int (*make)(void *,unsigned,unsigned,int,unsigned,void *,unsigned)=(void *)0x08063a36;
    int (*get)(void *,unsigned,unsigned,void *,unsigned,int *)=(void *)0x080645d0;
    for(unsigned upgrade=0;upgrade<32;++upgrade) for(unsigned refine=0;refine<=10;++refine)
        CHECK(make(avg,16400,upgrade,10000+(int)upgrade*100+(int)refine,0,roi,refine)==0);
    for(unsigned upgrade=0;upgrade<32;++upgrade) for(unsigned refine=0;refine<=10;++refine) {
        int value=-9; CHECK(get(avg,16400,upgrade,roi,refine,&value)==0);
        CHECK(value==10000+(int)upgrade*100+(int)refine);
    }
    int value=111;
    CHECK(get(avg,16400,0,roi,11,&value)==32 && value==111);
    CHECK(make(avg,16400,0,1,0,roi,11)==32);
    CHECK(get(avg,16400,32,roi,8,&value)==32);
    roi[1]=1; roi[2]=2;
    CHECK(make(avg,16400,0,314,0,roi,8)==0);
    CHECK(make(avg,16400,1,271,0,roi,0)==0);
    CHECK(get(avg,16400,0,roi,8,&value)==0 && value==314);
    CHECK(get(avg,16400,1,roi,0,&value)==0 && value==271);
    int (*add)(void *,unsigned,unsigned,int,unsigned char *,int *,unsigned,void *,unsigned *,unsigned)=(void *)0x08063c7a;
    unsigned char created=0; unsigned counter=0; int average=0;
    memset(roi,0,sizeof(roi));
    CHECK(add(avg,16401,0,777,&created,&average,1,roi,&counter,10)==0 && created==1);
    CHECK(get(avg,16401,0,roi,10,&value)==0 && value==777);
    CHECK(get(avg,16401,1,roi,2,&value)==0 && value==-1);
    CHECK(add(avg,16401,0,888,&created,&average,1,roi,&counter,11)==32);
    roi[1]=1; roi[2]=2;
    ((void (*)(void *))0x08064bd0)(avg);
    CHECK(get(avg,16400,0,roi,8,&value)==0 && value==314);
    ((void (*)(void *))0x080649e4)(avg);
    ((void (*)(void))0x0806b774)();
    unsigned char info[0x40]={0}; ((void (*)(void *))0x0807caae)(info);
    CHECK(((int (*)(void *,const char *,void *))0x0807cd9e)(info,"/utf8.dat",(void *)0x0806b7f4));
    char *found=((void *(*)(void *,int))0x0807d56e)(info,2675336);
    CHECK(found && !strcmp(*(char **)(found+0x18),"\xe6\xb5\x8b\xe8\xaf\x95\xe9\x87\x91\xe5\xb8\x81"));
    /* Real item registration, dictionary, search indexes and result serialization.
       This fixture constructs the original subobjects, without a DB/service main. */
    unsigned char factory[0x400]={0};
    ((void (*)(void *,char *,char *))0x080a957e)(factory,"fixture","fixture.cfg");
    *(void **)0x082cac90=factory;
    void *dict=owner+0x54, *indexed=owner+0x51d8, *catalog=owner+0x5274;
    ((void (*)(void *))0x08054e26)(dict); *(void **)((char *)dict+4)=owner;
    ((void (*)(void *))0x0807e450)(indexed); *(void **)((char *)indexed+4)=owner;
    ((void (*)(void *))0x0807caae)(catalog);
    CHECK(((int (*)(void *,const char *,void *))0x0807cd9e)(catalog,"/utf8.dat",(void *)0x0806b7f4));
    /* Odd UTF-8 byte lengths exposed the original DBCS tokenizer: it consumed
       the closing quote with the last Chinese byte and lost the category. */
    char *pants=((void *(*)(void *,int))0x0807d56e)(catalog,12400);
    CHECK(pants && *(unsigned short *)(pants+0x20)==11103);
    CHECK(!strcmp(*(char **)(pants+0x18),"\xe6\xae\x8b\xe7\xa0\xb4\xe7\x9a\x84\xe7\x89\x9b\xe7\x9a\xae\xe7\x9f\xad\xe8\xa3\xa4"));
    CHECK(!strcmp(*(char **)(pants+0x1c),"Cracked Leather Pants"));
    int (*regist)(void *,unsigned long long,int,const char *,int,int,unsigned,int,void *,long,int,const char *,void *,char,char *)=(void *)0x08051306;
    for(unsigned i=0;i<3;++i) {
        unsigned char item[0x40]={0},guid[10]={0}; unsigned empty_roi[3]={0};
        ((void (*)(void *))0x08056674)(item);
        *(unsigned *)(item+1)=i==2 ? 12400 : 16400+i;
        item[5]=i==2 ? 0 : i; item[0x2b]=i==2 ? 0 : (unsigned char)(8+i*2);
        CHECK(regist(dict,91000000001ull+i,100,"fixture",100,200,1,0,item,2147483647,0,"fixture",empty_roi,0,(char *)guid)==0);
    }
    int (*find_item)(void *,void *,void *,unsigned *,unsigned short *,void *)=(void *)0x08083142;
    int (*find_category)(void *,void *,unsigned *,unsigned short *,void *)=(void *)0x08083342;
    unsigned char req[64],results[137*60]; unsigned ids[128],total; unsigned short count;
    /* Exact ordinary-market request captured from the real client. Category
       11100 must include 11103; rarity 6 and level 0..0 mean all. */
    memset(req,0,sizeof(req)); memset(results,0,sizeof(results));
    *(unsigned short *)(req+4)=11100; req[6]=6; req[8]=31; req[24]=10;
    total=0; count=0;
    CHECK(find_category(indexed,req,&total,&count,results)==0 && total==3 && count==3);
    int found_pants=0;
    for(unsigned i=0;i<count;++i) if(*(unsigned *)(results+137*i+43)==12400) ++found_pants;
    CHECK(found_pants==1);
    for(unsigned i=0;i<2;++i) {
        memset(req,0,sizeof(req)); memset(results,0,sizeof(results));
        ids[0]=16400+i; req[5]=31;req[6]=1;req[22]=10;total=0;count=0;
        int result=find_item(indexed,req,ids,&total,&count,results);
        fprintf(stderr,"MARKET_METHOD item result=%d total=%u count=%u item=%u\n",result,total,count,16400+i);
        CHECK(result==0 && total==1 && count==1);
        CHECK(*(unsigned long long *)results==91000000001ull+i && *(unsigned *)(results+43)==16400+i);
        memset(req,0,sizeof(req)); *(unsigned short *)(req+4)=11105;req[6]=2;req[8]=31;
        req[9]=req[10]=(unsigned char)(i?100:85);req[24]=10;total=0;count=0;
        CHECK(find_category(indexed,req,&total,&count,results)==0 && total==1 && count==1);
        CHECK(*(unsigned long long *)results==91000000001ull+i);
    }
    req[24]=11; CHECK(find_category(indexed,req,&total,&count,results)==51);
    ((void (*)(void *))0x0807e562)(indexed);
    ((void (*)(void *))0x08054fd2)(dict);
    ((void (*)(void *))0x080a9842)(factory); *(void **)0x082cac90=NULL;
    memset(avg,0,sizeof(avg)); ((void (*)(void *))0x080647ae)(avg);
    roi[1]=roi[2]=0; CHECK(get(avg,16400,0,roi,8,&value)==0 && value==-1);
    ((void (*)(void *))0x080649e4)(avg);
    puts("MARKET_METHOD PASS actual registration + item/category results at level85/100 refine8/10; captured category11100 request includes item12400; all 352 upgrade/refine cells distinct; ROI cells distinct; add/get; invalid limits rejected; update/destruct/reuse; UTF8 odd/even Chinese names, spaced English name and category preserved");
    fflush(stdout); return 42;
}
void nr_probe_install(NrRun *run) {
    active=run; main_pc=sym("main");
    CHECK(!memcmp((void *)main_pc,"\x8d\x4c\x24\x04\x83\xe4",6));
    DWORD a,b; CHECK(VirtualProtect((void *)main_pc,2,PAGE_EXECUTE_READWRITE,&a));
    memcpy((void *)main_pc,"\x0f\x0b",2); CHECK(VirtualProtect((void *)main_pc,2,a,&b));
    CHECK(FlushInstructionCache(GetCurrentProcess(),(void *)main_pc,2));
}
int nr_probe_trap(EXCEPTION_POINTERS *p) {
    if(p->ExceptionRecord->ExceptionCode!=EXCEPTION_ILLEGAL_INSTRUCTION || p->ContextRecord->Eip!=main_pc) return 0;
    p->ContextRecord->Eip=(DWORD)(uintptr_t)fixture_main; return 1;
}
