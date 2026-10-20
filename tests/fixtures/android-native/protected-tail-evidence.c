#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef KL_X18_CORE_SOURCE
#include KL_X18_CORE_SOURCE
#else
#include KL_X18_SOURCE
#endif

static const uint32_t original[]={
    0x5281807cu,0x7a83a87du,0x2a85d475u,0xa9be7bf3u,
    0xd53bd053u,0xf9401668u,0xaa0003e1u,0xf9000be8u,
    0xd2808ad2u,0x8b120020u,0xf9401668u,0xf9400be9u,
    0xeb09011fu,0x54000061u,0xa8c27bf3u,0xd65f03c0u,
    0xd420e040u
};
enum {WORDS=sizeof original/sizeof *original};
static void spilled_pointer(void){
    const uint32_t source[]={
        0x5281807cu,0x7a83a87du,0x2a85d475u,0xa9bc7bfdu,
        0xa90153f3u,0x910003fdu,0xd10083ffu,
        0xd53bd048u,0xf81c03a8u,0xf9401508u,0xf81f83a8u,
        0xd10083ffu,0xd2800028u,0xf85c03a8u,0xf9401508u,
        0xf85f83a9u,0x7100027fu,0x1a9f07eau,0xeb09011fu,
        0x54000081u,0x910003bfu,0xa94153f3u,0xa8c47bfdu,0xd65f03c0u,
        0xd420e040u
    };
    enum{N=sizeof source/sizeof *source};uint32_t words[N];uint8_t state[N];
    const uint64_t roots[]={0x4000,0x4000+sizeof source};
    memcpy(words,source,sizeof words);memset(state,0,sizeof state);
    assert(!klx_instruction_evidence(words,N,0x4000,roots,2,state));
    assert(state[7]&KLX_EVIDENCE_CODE);assert(!memcmp(words,source,sizeof words));
    // Reloading a different slot, replacing the retained TP, or clobbering a
    // live guard before its comparison must not prove the opaque continuation.
    const struct{unsigned at;uint32_t word;} invalid[]={
        {13,0xf85c83a8u},{12,0xf81c03bfu},{17,0x1a9f07e8u},
        {15,0xf85f03a9u},{16,0x14000001u}
    };
    for(size_t i=0;i<sizeof invalid/sizeof *invalid;i++){
        memcpy(words,source,sizeof words);memset(state,0,sizeof state);words[invalid[i].at]=invalid[i].word;
        assert(!klx_instruction_evidence(words,N,0x4000,roots,2,state));assert(!(state[7]&KLX_EVIDENCE_CODE));
    }
    memcpy(words,source,sizeof words);memset(state,0,sizeof state);state[8]=KLX_EVIDENCE_DATA;
    assert(!klx_instruction_evidence(words,N,0x4000,roots,2,state));assert(!(state[7]&KLX_EVIDENCE_CODE));
}
static void original_prefix(void){
    const uint32_t source[]={0xd10203ffu,0xa90467feu,0xa9055ff8u,0xa90657f6u,0xa9074ff4u,
        0xd53bd056u,0xf94016c8u,0xaa0203f3u,0x7a839c6bu,0x2a81fc7cu};
    enum{N=sizeof source/sizeof *source};uint32_t words[N];uint8_t state[N];
    const uint64_t roots[]={0x8000,0x8000+sizeof source};
    memcpy(words,source,sizeof words);memset(state,0,sizeof state);
    assert(!klx_instruction_evidence(words,N,0x8000,roots,2,state));
    for(unsigned i=0;i<7;i++)assert(state[i]&KLX_EVIDENCE_CODE);
    for(unsigned i=7;i<N;i++)assert(!(state[i]&KLX_EVIDENCE_CODE));
    assert(!memcmp(words,source,sizeof words));
    const struct{unsigned at;uint32_t word;} invalid[]={
        {1,0xa90467fcu},{3,0xa90657f4u},{1,0xa91067feu},
        {6,0xf94012c8u},{0,0xd10207ffu},{6,0xf94016dfu}
    };
    for(size_t i=0;i<sizeof invalid/sizeof *invalid;i++){
        memcpy(words,source,sizeof words);memset(state,0,sizeof state);words[invalid[i].at]=invalid[i].word;
        assert(!klx_instruction_evidence(words,N,0x8000,roots,2,state));assert(!(state[5]&KLX_EVIDENCE_CODE));
    }
    memcpy(words,source,sizeof words);memset(state,0,sizeof state);state[6]=KLX_EVIDENCE_DATA;
    assert(!klx_instruction_evidence(words,N,0x8000,roots,2,state));assert(!(state[5]&KLX_EVIDENCE_CODE));
    memset(state,0,sizeof state);
    assert(!klx_instruction_evidence(words,N,0x8000,roots+1,1,state));assert(!(state[5]&KLX_EVIDENCE_CODE));
}
static void evidence(uint32_t *words,uint8_t *state,int valid){
    const uint64_t roots[]={0x2000+WORDS*4,0x2000,0x2000};
    assert(!klx_instruction_evidence(words,WORDS,0x2000,roots,3,state));
    assert(!state[0]&&!state[1]&&!state[2]);
    assert(!!(state[4]&KLX_EVIDENCE_CODE)==valid);
    if(valid){
        for(unsigned k=4;k<=15;k++)assert(state[k]&KLX_EVIDENCE_CODE);
        assert(!kl_x18_is_data_context(words,sizeof original,4,
            &(klx_code_context){NULL,0,0x2000,
                (klx_instruction_range[]){{0x2010,0x2040}},1}));
    }
    for(unsigned k=0;k<WORDS;k++)assert(!(state[k]&24));
}
int main(void){
    spilled_pointer();original_prefix();
    uint32_t words[WORDS];uint8_t state[WORDS];
    memcpy(words,original,sizeof words);memset(state,0,sizeof state);
    assert(kl_x18_is_data(words,sizeof words,4));
    evidence(words,state,1);assert(!memcmp(words,original,sizeof words));
    // The structural proof must agree about both stack slots and canary
    // offsets. A random TP-looking constant or damaged epilogue is not code.
    const struct {unsigned at;uint32_t value;} invalid[]={
        {5,0xf9401268u},{7,0xf9000fe8u},{10,0xf9401268u},
        {11,0xf9400fe9u},{12,0xeb0a011fu},{13,0x54000060u},
        {14,0x14000002u},{15,0xd503201fu},{6,0xaa0003f3u},
        {6,0xaa0003e8u},{9,0xaa0003f3u},
        {9,0x7a83a87du}
    };
    for(unsigned n=0;n<sizeof invalid/sizeof *invalid;n++){
        memcpy(words,original,sizeof words);memset(state,0,sizeof state);
        words[invalid[n].at]=invalid[n].value;evidence(words,state,0);
    }
    // The same complete original contract may spill the canary below FP,
    // and may preserve the TP in a stack slot before loading its canary.
    memcpy(words,original,sizeof words);memset(state,0,sizeof state);
    words[7]=0xf81f83a8u;words[11]=0xf85f83a9u;
    evidence(words,state,1);
    words[11]=0xf85f03a9u;memset(state,0,sizeof state);evidence(words,state,0);
    memcpy(words,original,sizeof words);memset(state,0,sizeof state);
    words[5]=0xf81c03b3u;words[6]=0xf9401668u;
    evidence(words,state,1);
    words[5]=0xaa0003f3u;memset(state,0,sizeof state);evidence(words,state,0);
    // Explicit ELF data and original neighboring function roots remain hard
    // boundaries; a continuation cannot borrow another function's return.
    memcpy(words,original,sizeof words);memset(state,0,sizeof state);
    state[5]=KLX_EVIDENCE_DATA;evidence(words,state,0);
    memset(state,0,sizeof state);const uint64_t separated[]={0x2000,0x2038};
    assert(!klx_instruction_evidence(words,WORDS,0x2000,separated,2,state));
    assert(!(state[4]&KLX_EVIDENCE_CODE));
    puts("PASS original canary continuation, opaque prefix, explicit data and function boundaries in shared instruction evidence");
}
