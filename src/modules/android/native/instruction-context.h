#pragma once
#include <stddef.h>
#include <stdint.h>

/* One ELF-owned context supplies instruction boundaries to every adaptation
 * pass. Boundaries narrow neighborhood votes; they do not authorize data as
 * code or replace the original decoder. */
typedef struct {uint64_t start,end;} klx_instruction_range;
typedef struct {
    const uint64_t *boundaries;size_t count;uint64_t address;
    const klx_instruction_range *reachable;size_t reachable_count;
} klx_code_context;
int kl_x18_is_data_context(const void *,size_t,size_t,const klx_code_context *);
/* State bytes: bit0 reachable code, bit1 explicit/inferred data, bit2 memory
 * reference. Roots are original virtual instruction addresses. */
int klx_instruction_evidence(const uint32_t *,size_t,uint64_t,const uint64_t *,size_t,uint8_t *);
/* Canonical AArch64 FP/LR save and matching frame establishment. Shared by
 * both ELF owners and the raw executable-region analysis. */
static inline int klj_code_frame_record(uint32_t save,uint32_t frame) {
    if((frame&0xff8003ffu)!=0x910003fdu)return 0;
    uint32_t offset=((frame>>10)&4095u)<<((frame&(1u<<22))?12:0);
    int displacement=(int)((save>>15)&127u);if(displacement&64)displacement-=128;
    displacement*=8;uint32_t form=save&0xffc07fffu;
    if(form==0xa9007bfdu)return displacement>=0&&offset==(uint32_t)displacement;
    return form==0xa9807bfdu&&displacement<0&&offset==0;
}
