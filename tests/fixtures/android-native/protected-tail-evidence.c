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
    // Explicit ELF data and original neighboring function roots remain hard
    // boundaries; a continuation cannot borrow another function's return.
    memcpy(words,original,sizeof words);memset(state,0,sizeof state);
    state[5]=KLX_EVIDENCE_DATA;evidence(words,state,0);
    memset(state,0,sizeof state);const uint64_t separated[]={0x2000,0x2038};
    assert(!klx_instruction_evidence(words,WORDS,0x2000,separated,2,state));
    assert(!(state[4]&KLX_EVIDENCE_CODE));
    puts("PASS original canary continuation, opaque prefix, explicit data and function boundaries in shared instruction evidence");
}
