/* The original image owner includes this after its generated decoder API. */
static int klj_image_tables_patch_context(void *code,size_t size,KljInlinePool *pools,size_t count,const kl_x18_image *image,kl_x18_stats *stats,const klx_code_context *context){
    memset(stats,0,sizeof *stats);
    const uint32_t *words=code;size_t terminal=0;
    for(size_t i=0;i<size/4;i++)
        if(words[i]==0xd61f0240u&&!kl_x18_is_data_context(code,size,i,context))terminal++;
    if(!terminal||!image||!image->data_at)return 0;
    size_t page=(size_t)getpagesize();
    if(terminal>(SIZE_MAX-page+1)/(KLX_JT_MAX_INSN*4)){errno=ENOMEM;return -1;}
    size_t needed=(terminal*KLX_JT_MAX_INSN*4+page-1)&~(page-1);
    for(size_t n=0;n<count;n++){
        KljInlinePool *pool=&pools[n];
        if(!pool->base||pool->used>pool->size)continue;
        size_t capacity=pool->size-pool->used;
        if(capacity>needed)capacity=needed;
        if(!pool->packed)capacity&=~(page-1);
        if(capacity<16)continue;
        uintptr_t address=(uintptr_t)pool->base+pool->used;
        if(address<(uintptr_t)pool->base||capacity>UINTPTR_MAX-address||page-1>UINTPTR_MAX-address-capacity){errno=ENOMEM;return -1;}
        uintptr_t low=address&~(uintptr_t)(page-1),high=(address+capacity+page-1)&~(uintptr_t)(page-1);
        if(mprotect((void *)low,high-low,PROT_READ|PROT_WRITE))return -1;
        kl_x18_stats emitted;size_t used=0;
        if(kl_x18_emit_tables_context(code,size,(uintptr_t)code,(void *)address,capacity,address,image,&emitted,&used,context))return -1;
        if(used>capacity){errno=ENOMEM;return -1;}
        if(used){
            sys_icache_invalidate((void *)address,used);
            pool->used+=pool->packed?used:(used+page-1)&~(page-1);
        }
        stats->jt_sites+=emitted.jt_sites;
        stats->jt_thunks+=emitted.jt_thunks;
    }
    return 0;
}
static int x18_body(uint32_t word,uintptr_t site,uintptr_t target,uint32_t *body,size_t *bytes,uint32_t *entry){
    // Only a site accepted by the original full-range classifier reaches this
    // callback. Preserve its decision while the original emitter sees one word
    // at the same original PC, and validates its complete future veneer body.
    klx_instruction_range reachable={site,site+4};
    klx_code_context accepted={NULL,0,site,&reachable,1};
    kl_x18_stats stats;size_t used=0;uint32_t temporary=word;
    int result=kl_x18_emit_context(&temporary,4,site,body,12*4,target,&stats,&used,&accepted);
    if(result||stats.refused||stats.ctr_refused||!(stats.patched+stats.ctr_patched)){
        errno=ENOEXEC;return -1;
    }
    *entry=temporary;*bytes=used;return 0;
}
static int image_body(uint32_t word,uintptr_t site,uintptr_t target,uint32_t *body,size_t *bytes,uint32_t *entry){
    return klj_inline_instruction_match(word)?klj_inline_instruction_body(word,site,target,body,bytes,entry):x18_body(word,site,target,body,bytes,entry);
}
static int instruction_pools(void *code,size_t size,uintptr_t address,KljInlinePool *pools,size_t pool_count,kl_x18_stats *stats,const klx_code_context *context,int include_inline,void *const *aliases){
    memset(stats,0,sizeof *stats);uint32_t *words=code;size_t count=0;
    for(size_t i=0;i<size/4;i++){
        klx_info info;klx_decode(words[i],&info);
        int inlined=include_inline&&klj_inline_instruction_match(words[i]);
        if(!inlined&&!info.nfields&&!info.sysreg)continue;
        if(kl_x18_is_data_context(code,size,i,context)){if(info.nfields)stats->data_words++;continue;}
        if(!inlined&&info.nfields)stats->sites++;
        if(!inlined&&info.sysreg)stats->ctr_sites++;
        if(!inlined&&info.hazard==KLX_HZ_TERMBR)stats->jt_fallback++;
        count++;
    }
    if(!count)return 0;
    if(count>SIZE_MAX/sizeof(KljInlineSite)){errno=ENOMEM;return -1;}
    KljInlineSite *sites=calloc(count,sizeof *sites);if(!sites)return -1;
    size_t next=0;
    for(size_t i=0;i<size/4;i++){
        klx_info info;klx_decode(words[i],&info);
        if(((include_inline&&klj_inline_instruction_match(words[i]))||info.nfields||info.sysreg)&&!kl_x18_is_data_context(code,size,i,context))sites[next++].word=i;
    }
    int result=klj_inline_plan_apply_aliases(code,address,sites,count,pools,pool_count,include_inline?image_body:x18_body,aliases);
    if(!result){stats->patched=stats->sites;stats->ctr_patched=stats->ctr_sites;}
    else{stats->refused=stats->sites;stats->ctr_refused=stats->ctr_sites;if(errno==ERANGE)stats->far_refused=stats->sites;}
    free(sites);return result;
}
int klj_x18_patch_pools_context(void *code,size_t size,KljInlinePool *pools,size_t count,kl_x18_stats *stats,const klx_code_context *context){return instruction_pools(code,size,(uintptr_t)code,pools,count,stats,context,0,NULL);}
int klj_image_instruction_patch_context(void *code,size_t size,KljInlinePool *pools,size_t count,kl_x18_stats *stats,const klx_code_context *context){return instruction_pools(code,size,(uintptr_t)code,pools,count,stats,context,1,NULL);}
int klj_image_instruction_emit_pools_context(void *code,size_t size,uintptr_t address,KljInlinePool *pools,size_t count,kl_x18_stats *stats,const klx_code_context *context,void *const *aliases){return instruction_pools(code,size,address,pools,count,stats,context,1,aliases);}
