/* C memset zero semantics using demand-zero private pages, not an omitted fill.
   Derive the PLT/GOT site from ELF metadata; immutable input is never patched. */
#include "run.h"
#include <stdio.h>
#include <string.h>

static int section_bytes(const NrElf *elf, const NrElfSection *s) {
    return s->offset<=elf->file_size && s->size<=elf->file_size-s->offset;
}

int nr_zero_adapt(NrElf *elf, const char **error) {
    const NrElfHeader *h=elf->header;
    if(!h->shnum || h->shstrndx>=h->shnum || h->shentsize!=sizeof(NrElfSection) ||
       h->shoff>elf->file_size || (size_t)h->shnum*sizeof(NrElfSection)>elf->file_size-h->shoff) return 1;
    const NrElfSection *sections=(const NrElfSection *)(elf->file+h->shoff), *plt=NULL;
    const NrElfSection *names=&sections[h->shstrndx];
    if(!section_bytes(elf,names)) return 1;
    for(unsigned i=0;i<h->shnum;++i) {
        const NrElfSection *s=&sections[i];
        if(s->name<names->size && names->size-s->name>=5 &&
           !memcmp(elf->file+names->offset+s->name,".plt",5)) plt=s;
    }
    if(!plt || !(plt->flags&4) || !section_bytes(elf,plt)) return 1;
    for(unsigned i=0;i<h->shnum;++i) {
        const NrElfSection *rel=&sections[i];
#ifdef _WIN64
        const size_t rel_size=24, symbol_size=24;
        if(rel->type!=4) continue; /* Elf64_Rela */
#else
        const size_t rel_size=8, symbol_size=16;
        if(rel->type!=9) continue; /* Elf32_Rel */
#endif
        if(rel->entsize!=rel_size || !section_bytes(elf,rel) || rel->link>=h->shnum) continue;
        const NrElfSection *symbols=&sections[rel->link];
        if(symbols->type!=11 || symbols->entsize!=symbol_size || !section_bytes(elf,symbols) || symbols->link>=h->shnum) continue;
        const NrElfSection *strings=&sections[symbols->link];
        if(!section_bytes(elf,strings)) continue;
        for(size_t offset=0;offset+rel_size<=rel->size;offset+=rel_size) {
            uintptr_t got; size_t index; unsigned type;
#ifdef _WIN64
            uint64_t fields[2]; memcpy(fields,elf->file+rel->offset+offset,sizeof(fields));
            got=(uintptr_t)fields[0];index=(size_t)(fields[1]>>32);type=(unsigned)fields[1];
#else
            uint32_t fields[2]; memcpy(fields,elf->file+rel->offset+offset,sizeof(fields));
            got=fields[0];index=fields[1]>>8;type=fields[1]&255;
#endif
            if(type!=7 || index>=symbols->size/symbol_size) continue; /* JUMP_SLOT */
            uint32_t name; memcpy(&name,elf->file+symbols->offset+index*symbol_size,4);
            if(name>=strings->size || strings->size-name<7 || memcmp(elf->file+strings->offset+name,"memset",7)) continue;
            for(size_t pos=16;pos+6<=plt->size;pos+=16) {
                const unsigned char *bytes=elf->file+plt->offset+pos;
                if(bytes[0]!=0xff || bytes[1]!=0x25) continue;
                uint32_t operand; memcpy(&operand,bytes+2,4);
                uintptr_t pc=elf->bias+(uintptr_t)plt->addr+pos;
#ifdef _WIN64
                uintptr_t target=pc+6+(intptr_t)(int32_t)operand;
#else
                uintptr_t target=operand;
#endif
                if(target!=elf->bias+got || pc<(uintptr_t)elf->allocation ||
                   pc-(uintptr_t)elf->allocation>elf->allocation_size-6 ||
                   target<(uintptr_t)elf->allocation || target-(uintptr_t)elf->allocation>elf->allocation_size-sizeof(uintptr_t)) continue;
                *error="memset PLT original bytes differ";
                if(memcmp((const void *)pc,bytes,6)) return 0;
                DWORD old,ignored;
                if(!VirtualProtect((void *)pc,2,PAGE_EXECUTE_READWRITE,&old)) return 0;
                memcpy((void *)pc,"\x0f\x0b",2);
                if(!VirtualProtect((void *)pc,2,old,&ignored) || !FlushInstructionCache(GetCurrentProcess(),(void *)pc,2)) return 0;
                elf->zero_pc=pc;elf->zero_got=target;
                fprintf(stderr,"zero-page memset sha256=%s pc=0x%llx original=ff25%02x%02x%02x%02x replacement=0f0b scope=execution-memory restore=original-file\n",
                        elf->sha256,(unsigned long long)pc,bytes[2],bytes[3],bytes[4],bytes[5]);
                *error=NULL;return 1;
            }
        }
    }
    return 1;
}

static int libc_target(const NrRun *run, uintptr_t target) {
    for(unsigned i=0;i<run->vm_count;++i) {
        const NrVm *v=&run->vm[i];
        if(!v->image.file || !v->path || target<(uintptr_t)v->base || target-(uintptr_t)v->base>=v->size) continue;
        const char *name=strrchr(v->path,'/');
        if(name && (!strcmp(name,"/libc.so.6") || !strcmp(name,"/libc-2.17.so"))) return 1;
    }
    return 0;
}

static int zero_pages(NrRun *run, uintptr_t address, size_t size) {
    if(size<65536 || address>UINTPTR_MAX-size || !nr_memory(address,size,1)) return 0;
    NrVm *owner=NULL;
    for(unsigned i=0;i<run->vm_count;++i) {
        NrVm *v=&run->vm[i];
        if(!v->image.file && !v->section && v->base && address>=(uintptr_t)v->base &&
           address-(uintptr_t)v->base<=v->size && size<=v->size-(address-(uintptr_t)v->base)) {owner=v;break;}
    }
    if(!owner) return 0;
    uintptr_t end=address+size, first=(address+4095)&~(uintptr_t)4095, last=end&~(uintptr_t)4095;
    if(first>=last) return 0;
    /* Check the entire range before changing it; only plain writable anonymous
       pages qualify. Shared/file pages, guards and executable pages use libc. */
    for(uintptr_t cursor=first;cursor<last;) {
        MEMORY_BASIC_INFORMATION info;
        if(!VirtualQuery((const void *)cursor,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
           info.Type!=MEM_PRIVATE || info.Protect!=PAGE_READWRITE) return 0;
        uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;
        cursor=next<last?next:last;
    }
    /* The lock hides the transient decommit, and success restores exactly the
       committed private RW pages checked above. Cached permissions stay valid;
       discarding unrelated entries here makes each user reset rescan its pool. */
    if(!VirtualFree((void *)first,last-first,MEM_DECOMMIT) ||
       !VirtualAlloc((void *)first,last-first,MEM_COMMIT,PAGE_READWRITE)) {
        fprintf(stderr,"zero-page memset failed address=0x%llx bytes=%zu win32=%lu\n",
                (unsigned long long)first,(size_t)(last-first),GetLastError());return -1;
    }
    memset((void *)address,0,first-address);
    memset((void *)last,0,end-last);
    return 1;
}

int nr_zero_fill(NrRun *run, CONTEXT *c) {
#ifdef _WIN64
    uintptr_t pc=c->Rip,sp=c->Rsp;
#else
    uintptr_t pc=c->Eip,sp=c->Esp;
#endif
    if(!run->program.zero_pc || pc!=run->program.zero_pc) return 0;
    EnterCriticalSection(&run->memory_lock);
    if(!nr_memory(sp,sizeof(uintptr_t),0) || !nr_memory(run->program.zero_got,sizeof(uintptr_t),0)) {
        LeaveCriticalSection(&run->memory_lock);return -1;
    }
    uintptr_t target=*(const uintptr_t *)run->program.zero_got, address=0,value=1,size=0;
#ifdef _WIN64
    address=c->Rdi;value=c->Rsi;size=c->Rdx;
#else
    if(nr_memory(sp,4*sizeof(uintptr_t),0)) {
        const uintptr_t *args=(const uintptr_t *)sp;address=args[1];value=args[2];size=args[3];
    }
#endif
    /* Let the dynamic linker resolve the first call normally, and honor any
       interposed memset outside libc instead of bypassing its behavior. */
    int result=!(value&255) && libc_target(run,target)?zero_pages(run,address,size):0;
    if(result>0) {
#ifdef _WIN64
        c->Rip=*(const uintptr_t *)sp;c->Rsp+=8;c->Rax=address;
#else
        c->Eip=*(const uintptr_t *)sp;c->Esp+=4;c->Eax=address;
#endif
    } else if(!result) {
#ifdef _WIN64
        c->Rip=target;
#else
        c->Eip=target;
#endif
    }
    LeaveCriticalSection(&run->memory_lock);
    return result<0?-1:1;
}
