#pragma once
/* The original ELF owners provide roots and explicit data. This small shared
 * pass adds evidence, not an instruction admission list: unknown code remains
 * eligible. The Klepton decoder owns GP operand/write semantics and its data
 * neighborhood detector; this pass follows A64 control flow and PC addresses.
 * CODE wins over inferred literal data. */
#define KLX_EVIDENCE_CODE 1u
#define KLX_EVIDENCE_DATA 2u
#define KLX_EVIDENCE_READ 4u

static int64_t klj_a64_signed(uint32_t value,unsigned bits) {
    return (int64_t)(value^(1u<<(bits-1)))-(1LL<<(bits-1));
}
static size_t klj_evidence_index(uint64_t address,uint64_t base,size_t count) {
    if(address<base||(address-base)&3)return count;
    uint64_t index=(address-base)/4;return index<count?(size_t)index:count;
}
typedef struct {size_t target,begin,end;} klj_evidence_branch;
static int klj_evidence_branch_cmp(const void *a,const void *b) {
    size_t x=((const klj_evidence_branch *)a)->target,y=((const klj_evidence_branch *)b)->target;
    return x<y?-1:x>y;
}
/* A fixed FP slot can retain the compiler's TP even when its register is later
 * reused. The key is the exact matching 64-bit non-writeback load encoding. */
static uint32_t klj_evidence_fp_store(uint32_t word) {
    if(((word>>5)&31u)!=29u)return 0;
    return ((word&0xffc00000u)==0xf9000000u||(word&0xffe00c00u)==0xf8000000u)?
        (word|0x00400000u)&~31u:0;
}
static uint32_t klj_evidence_fp_load(uint32_t word) {
    if(((word>>5)&31u)!=29u)return 0;
    return ((word&0xffc00000u)==0xf9400000u||(word&0xffe00c00u)==0xf8400000u)?word&~31u:0;
}

/* A protected function can retain an opaque entry while its compiler-emitted
 * body is ordinary code. Its unwind root alone does not prove that body. The
 * complete Bionic canary contract does: a TP load, the original canary slot,
 * a saved stack slot, the same paired comparison and an intact return path.
 * This only supplies another control-flow root. The normal decoder still
 * decides each reachable word, and explicit ELF data never becomes code. */
static int klj_evidence_canary_tail(const uint32_t *words,size_t begin,size_t end,
                                    const uint8_t *state) {
    uint32_t read=words[begin];
    if(end-begin<7||(read&0xffffffe0u)!=0xd53bd040u||
       ((state[begin]|state[begin+1])&KLX_EVIDENCE_DATA))return 0;
    unsigned tp=read&31u,guard=31u;size_t loaded=end;uint32_t pointer_slot=0;
    if(tp==31)return 0;
    for(size_t k=begin+1;k<end&&k-begin<4;k++) {
        uint32_t w=words[k];klx_info info;klx_decode(w,&info);
        if(!info.ok||(state[k]&KLX_EVIDENCE_DATA))return 0;
        if((w&0xffffffe0u)==(0xf9401400u|(tp<<5))){guard=w&31u;loaded=k;break;}
        if((w&31u)==tp){uint32_t key=klj_evidence_fp_store(w);if(key)pointer_slot=key;}
        if((info.gp_written&(1u<<tp))||(w&0xfc000000u)==0x94000000u||
           (w&0xfffffc1fu)==0xd63f0000u)return 0;
    }
    if(guard==31u)return 0;
    size_t saved=end;uint32_t reload=0;unsigned stack_base=31;
    for(size_t k=loaded+1;k<end&&k-begin<10;k++) {
        uint32_t w=words[k];klx_info info;klx_decode(w,&info);
        if(!info.ok||(state[k]&KLX_EVIDENCE_DATA))return 0;
        unsigned base=(w>>5)&31u;
        if((base==31u||base==29u)&&(w&31u)==guard&&
           ((w&0xffc00000u)==0xf9000000u||(w&0xffe00c00u)==0xf8000000u)) {
            // STR/LDR unsigned and STUR/LDUR signed offsets must use the same
            // stable SP/FP slot. Register-offset and writeback forms do not
            // establish a canary slot.
            reload=(w|0x00400000u)&~31u;stack_base=base;saved=k;break;
        }
        if((info.gp_written&((1u<<guard)|(1u<<tp)))||(w&0xfc000000u)==0x94000000u||
           (w&0xfffffc1fu)==0xd63f0000u)return 0;
    }
    if(saved==end)return 0;
    uint32_t pointers=tp==guard?0u:1u<<tp;
    for(size_t k=saved+1;k<end;k++) {
        uint32_t w=words[k];klx_info info;klx_decode(w,&info);
        if(!info.ok||(state[k]&KLX_EVIDENCE_DATA))return 0;
        unsigned base=(w>>5)&31u;
        if(end-k>=4&&(w&0xffc00000u)==0xf9400000u&&base<31&&
           (pointers&(1u<<base))&&((w>>10)&4095u)==5u) {
            unsigned live=w&31u,stack=words[k+1]&31u;
            size_t comparison=k+2;
            if(live!=31&&stack!=31&&live!=stack&&(words[k+1]&~31u)==reload) {
            for(;comparison<end&&comparison-k<6;comparison++) {
                uint32_t compare=words[comparison];klx_info compared;klx_decode(compare,&compared);
                if(!compared.ok||(state[comparison]&KLX_EVIDENCE_DATA))return 0;
                if(
               (compare&0xffe0fc1fu)==0xeb00001fu&&
               ((((compare>>5)&31u)==live&&((compare>>16)&31u)==stack)||
                (((compare>>5)&31u)==stack&&((compare>>16)&31u)==live)))break;
                // Result/flag calculations may intervene, but neither guard
                // value nor control flow can change before their comparison.
                if((compared.gp_written&((1u<<live)|(1u<<stack)))||
                   (compare&0x7c000000u)==0x14000000u||
                   (compare&0xff000010u)==0x54000000u||
                   (compare&0x7e000000u)==0x34000000u||
                   (compare&0x7e000000u)==0x36000000u||((compare>>25)&0x7fu)==0x6bu)return 0;
            }
            if(comparison+1<end&&comparison-k<6&&(words[comparison+1]&0xff00001fu)==0x54000001u) {
                for(size_t tail=comparison+2;tail<end;tail++) {
                    uint32_t next=words[tail];klx_decode(next,&info);
                    if(!info.ok||(state[tail]&KLX_EVIDENCE_DATA))return 0;
                    if(next==0xd65f03c0u)return 1;
                    /* The successful epilogue must reach its own return,
                     * not borrow a different function's terminal word. */
                    if((next&0x7c000000u)==0x14000000u||
                       (next&0xff000010u)==0x54000000u||
                       (next&0x7e000000u)==0x34000000u||
                       (next&0x7e000000u)==0x36000000u||
                       ((next>>25)&0x7fu)==0x6bu)return 0;
                }
                return 0;
            }
            }
        }
        if((stack_base==29u||pointer_slot)&&(info.gp_written&(1u<<29)))return 0;
        uint32_t stored=klj_evidence_fp_store(w),loaded_slot=klj_evidence_fp_load(w);
        if(stored){
            if(pointers&(1u<<(w&31u)))pointer_slot=stored;
            else if(pointer_slot==stored)pointer_slot=0;
        }
        pointers&=~info.gp_written;
        if(pointer_slot&&loaded_slot==pointer_slot&&(w&31u)!=31u)pointers|=1u<<(w&31u);
        if((w&0xffffffe0u)==0xd53bd040u&&((w&31u)!=31u))pointers|=1u<<(w&31u);
        if((w&0xfc000000u)==0x94000000u||(w&0xfffffc1fu)==0xd63f0000u)
            pointers&=0x3ff80000u; /* AAPCS callee-saved x19..x29. */
    }
    return 0;
}
/* An original function can cross into an opaque page after its ordinary
 * prologue. Prove only that prefix: an allocated aligned call frame, retained
 * original LR/callee registers, then the exact Bionic TP/canary load. No byte of
 * its opaque continuation acquires code evidence from this contract. */
static size_t klj_evidence_canary_prefix(const uint32_t *words,size_t begin,size_t end,
                                        const uint8_t *state) {
    int64_t sp=0,slots[32];unsigned registers[32];size_t slot_count=0;
    uint32_t saved=0;
    for(size_t k=begin;k<end&&k-begin<16;k++) {
        uint32_t word=words[k];klx_info info;klx_decode(word,&info);
        if(!info.ok||(state[k]&KLX_EVIDENCE_DATA))return begin;
        if((word&0xffffffe0u)==0xd53bd040u) {
            unsigned tp=word&31u;
            if(sp>=0||tp==31||!(saved&(1u<<30))||
               (tp>=19&&!(saved&(1u<<tp)))||k+1>=end||
               (state[k+1]&KLX_EVIDENCE_DATA)||
               (words[k+1]&0xffffffe0u)!=(0xf9401400u|(tp<<5))||
               (words[k+1]&31u)==31u)return begin;
            return k+2;
        }
        if((word&0xff8003ffu)==0xd10003ffu) {
            uint64_t bytes=((uint64_t)(word>>10)&4095u)<<((word&(1u<<22))?12:0);
            if(!bytes||(bytes&15u)||bytes>(uint64_t)(INT64_MAX+sp))return begin;
            sp-=(int64_t)bytes;continue;
        }
        if((word&0xff8003ffu)==0x910003fdu) {
            uint64_t offset=((uint64_t)(word>>10)&4095u)<<((word&(1u<<22))?12:0);
            if(sp>=0||offset>(uint64_t)-sp)return begin;
            continue;
        }
        unsigned count=0,reg[2]={word&31u,(word>>10)&31u};int64_t offset=0;
        uint32_t form=word&0xffc003e0u;
        if(form==0xa98003e0u||form==0xa90003e0u) {
            offset=klj_a64_signed((word>>15)&127u,7)*8;count=2;
            if(form==0xa98003e0u){if(offset>=0||(offset&15))return begin;sp+=offset;offset=0;}
        }else if(form==0xf90003e0u){offset=((word>>10)&4095u)*8;count=1;}
        else return begin;
        if(sp>=0||offset<0||offset+(int64_t)count*8>-sp)return begin;
        for(unsigned n=0;n<count;n++) {
            if(reg[n]<19||reg[n]>30||slot_count==32)return begin;
            int64_t slot=sp+offset+n*8;
            for(size_t old=0;old<slot_count;old++)if(slots[old]==slot)saved&=~(1u<<registers[old]);
            slots[slot_count]=slot;registers[slot_count++]=reg[n];saved|=1u<<reg[n];
        }
    }
    return begin;
}
int klx_instruction_evidence(const uint32_t *words,size_t count,uint64_t base,
                            const uint64_t *roots,size_t root_count,uint8_t *state) {
    if(count>SIZE_MAX/(2*sizeof(size_t)))return -1;
    size_t *queue=malloc((count?count:1)*2*sizeof *queue);if(!queue)return -1;
    size_t *reads=queue+count;for(size_t k=0;k<count;k++)reads[k]=count;
    size_t head=0,tail=0,branch_count=0,branch_capacity=0;
    klj_evidence_branch *branches=NULL;
    /* Bit 8 is queued; each word is queued at most once, including cycles. */
#define KLJ_QUEUE(address) do {size_t queued_index=klj_evidence_index((address),base,count); \
    if(queued_index<count&&!(state[queued_index]&(8u|KLX_EVIDENCE_DATA))){state[queued_index]|=8u;queue[tail++]=queued_index;}}while(0)
#define KLJ_BRANCH(address) do {size_t branch_target_index=klj_evidence_index((address),base,count); \
    if(branch_target_index<count){ \
        if(branch_count==branch_capacity){size_t next=branch_capacity?branch_capacity*2:64; \
            if(next<branch_capacity||next>SIZE_MAX/sizeof *branches){free(branches);free(queue);return -1;} \
            void *grown=realloc(branches,next*sizeof *branches);if(!grown){free(branches);free(queue);return -1;} \
            branches=grown;branch_capacity=next;} \
        branches[branch_count++]=(klj_evidence_branch){branch_target_index,begin,at+1}; \
    }}while(0)
    for(size_t k=0;k<root_count;k++) {
        size_t index=klj_evidence_index(roots[k],base,count);
        if(index<count)state[index]|=16u;
        KLJ_QUEUE(roots[k]);
    }
    for(size_t k=0;k+1<count;k++)if(klj_code_frame_record(words[k],words[k+1])) {
        state[k]|=16u;KLJ_QUEUE(base+k*4);
    }
    /* Original roots bound each recovery, even when supplied out of order or
     * repeated. Internal bit16 avoids another ELF-size allocation/sort. The
     * canary is a prologue operation; only its first few instructions can
     * establish this seed, never an arbitrary interior constant table. */
    size_t function=count;
    for(size_t end=0;end<=count;end++)if(end==count||(state[end]&16u)) {
        if(function<count)for(size_t k=function;k<end&&k-function<16;k++)
            if(!(state[k]&KLX_EVIDENCE_DATA)&&
               klj_evidence_canary_tail(words,k,end,state))KLJ_QUEUE(base+k*4);
        function=end;
    }
    while(head<tail) {
        size_t at=queue[head++],begin=at;int complete=0;
        uint64_t addresses[31]={0};uint32_t known=0;
        for(;at<count;at++) {
            if(state[at]&KLX_EVIDENCE_CODE){complete=1;break;}
            if(state[at]&KLX_EVIDENCE_DATA)break;
            uint32_t w=words[at];uint64_t pc=base+at*4;
            if(!w)break; /* UDF/padding, no fallthrough */
            klx_info info;klx_decode(w,&info);
            /* This is evidence propagation, not execution coverage. Unknown
             * encodings end this inferred path; the normal scanner still
             * handles ambiguous words, including unsupported extensions. */
            if(!info.ok)break;
            state[at]|=KLX_EVIDENCE_CODE;
            unsigned rd=w&31,rn=(w>>5)&31;
            if((w&0x7c000000u)==0x14000000u) {
                KLJ_QUEUE(pc+(uint64_t)(klj_a64_signed(w&0x3ffffffu,26)*4));
                if(!(w&0x80000000u)){
                    /* An in-range destination can still be packed data.
                     * Prove this prefix only when the destination reaches
                     * decoded code, through the same dependency graph as
                     * conditional branches. */
                    KLJ_BRANCH(pc+(uint64_t)(klj_a64_signed(w&0x3ffffffu,26)*4));
                    at++;break; /* B; BL also returns */
                }
                known&=0x3ff80000u; /* AAPCS callee-saved x19..x29 */
                continue;
            }
            if((w&0xff000010u)==0x54000000u||(w&0x7e000000u)==0x34000000u) {
                uint64_t target=pc+(uint64_t)(klj_a64_signed((w>>5)&0x7ffffu,19)*4);
                KLJ_QUEUE(target);KLJ_BRANCH(target);continue;
            }
            if((w&0x7e000000u)==0x36000000u) {
                uint64_t target=pc+(uint64_t)(klj_a64_signed((w>>5)&0x3fffu,14)*4);
                KLJ_QUEUE(target);KLJ_BRANCH(target);continue;
            }
            /* Complete ordinary/PA branch-register family is validated by the
             * existing decoder; never follow branch-shaped constant words. */
            if(((w>>25)&0x7f)==0x6b&&info.ok) {
                if(rn<31&&(known&(1u<<rn)))KLJ_QUEUE(addresses[rn]);
                if((w&0x00200000u)==0){complete=1;at++;break;} /* BR/RET, including PA */
                known&=0x3ff80000u;continue;
            }
            if((w&0x3b000000u)==0x18000000u) {
                size_t k=klj_evidence_index(pc+(uint64_t)(klj_a64_signed((w>>5)&0x7ffffu,19)*4),base,count);
                if(k<count)reads[at]=k;
            }
            /* ADR(P) addresses are data only after an actual memory operand
             * consumes them. Merely taking a function address is not data. */
            if((w&0x3b000000u)!=0x18000000u&&(w&0x0a000000u)==0x08000000u&&(w&(1u<<22))&&rn<31&&(known&(1u<<rn))) {
                uint64_t address=addresses[rn];
                if((w&0x3b000000u)==0x39000000u)address+=((w>>10)&4095u)<<(w>>30);
                else if((w&0x3a000000u)==0x28000000u&&((w>>23)&3)!=1)
                    address+=(uint64_t)(klj_a64_signed((w>>15)&127u,7)*(1u<<(((w>>30)==2)?3:2)));
                else if((w&0x3b200000u)==0x38000000u&&((w>>10)&3)!=1)
                    address+=(uint64_t)klj_a64_signed((w>>12)&511u,9);
                size_t k=klj_evidence_index(address,base,count);if(k<count)reads[at]=k;
            }
            uint64_t value=0;int defines=0;
            if((w&0x1f000000u)==0x10000000u) {
                int64_t immediate=klj_a64_signed(((w>>5)&0x7ffffu)*4+((w>>29)&3),21);
                value=(w&0x80000000u)?(pc&~4095ULL)+(uint64_t)(immediate*4096):pc+(uint64_t)immediate;
                defines=1;
            } else if((w&0xff000000u)==0x91000000u&&rn<31&&(known&(1u<<rn))) {
                value=addresses[rn]+(((uint64_t)(w>>10)&4095u)<<((w&(1u<<22))?12:0));defines=1;
            } else if((w&0xffe0ffe0u)==0xaa0003e0u) {
                unsigned rm=(w>>16)&31;if(rm<31&&(known&(1u<<rm))){value=addresses[rm];defines=1;}
            }
            if(info.ok)known&=~info.gp_written;else known=0;
            if(defines&&rd<31){addresses[rd]=value;known|=1u<<rd;}
        }
        if(!complete)for(size_t k=begin;k<at;k++)state[k]&=~KLX_EVIDENCE_CODE;
    }
    // A complete rooted prologue establishes only its own prefix, even when
    // the ordinary decoded path stops in a later opaque page. This runs after
    // fallthrough analysis so incomplete continuations cannot erase that fact.
    function=count;
    for(size_t end=0;end<=count;end++)if(end==count||(state[end]&16u)) {
        if(function<count){size_t prefix=klj_evidence_canary_prefix(words,function,end,state);
            for(size_t k=function;k<prefix;k++)state[k]|=KLX_EVIDENCE_CODE;}
        function=end;
    }
    /* A valid conditional path can skip opaque inline bytes. Its decoded
     * prefix remains code when the target reaches established code, even if
     * fallthrough stops at an unknown word. Do not promote opaque bytes or
     * mere unwind roots. Each newly proven word is queued once; branch edges
     * are sparse and indexed by their original target. */
    qsort(branches,branch_count,sizeof *branches,klj_evidence_branch_cmp);
    head=tail=0;for(size_t k=0;k<count;k++)if(state[k]&KLX_EVIDENCE_CODE)queue[tail++]=k;
    while(head<tail){
        size_t target=queue[head++],lo=0,hi=branch_count;
        while(lo<hi){size_t mid=lo+(hi-lo)/2;if(branches[mid].target<target)lo=mid+1;else hi=mid;}
        for(size_t edge=lo;edge<branch_count&&branches[edge].target==target;edge++)
            for(size_t k=branches[edge].begin;k<branches[edge].end;k++)
                if(!(state[k]&KLX_EVIDENCE_CODE)){state[k]|=KLX_EVIDENCE_CODE;queue[tail++]=k;}
    }
    for(size_t k=0;k<count;k++)if((state[k]&KLX_EVIDENCE_CODE)&&reads[k]<count)state[reads[k]]|=KLX_EVIDENCE_READ;
    /* Only an unreachable interval actually referenced as memory can supply
     * new exclusions. The upstream neighborhood detector limits those to
     * data-like words; unreferenced/ambiguous code is not silently omitted. */
    for(size_t start=0;start<count;) {
        if(state[start]&KLX_EVIDENCE_CODE){start++;continue;}
        size_t end=start,first_read=count;
        while(end<count&&!(state[end]&KLX_EVIDENCE_CODE)){
            if(first_read==count&&(state[end]&KLX_EVIDENCE_READ))first_read=end;
            end++;
        }
        for(size_t k=first_read;k<end;k++)
            if(klj_x18_data_context(words,count,k,NULL))state[k]|=KLX_EVIDENCE_DATA;
        start=end;
    }
    for(size_t k=0;k<count;k++)state[k]&=~24u;
    free(branches);free(queue);return 0;
#undef KLJ_QUEUE
#undef KLJ_BRANCH
}
