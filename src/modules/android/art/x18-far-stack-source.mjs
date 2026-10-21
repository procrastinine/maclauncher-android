/** Keep the original validated interrupted-context completion for far bodies. */
export function farStackResumeSource(original){
 let text=original;
 const edit=(from,to)=>{if(text.split(from).length!==2)throw Error('Original far stack completion boundary changed');text=text.replace(from,to);};
 text=text.replaceAll('klj_x18_copy','klj_x18_far_copy');
 edit('static int klj_x18_stack_resume(void *context)', 'static int klj_x18_stack_resume_far(void *context,uint64_t target)');
 edit('(words[8]&0xfc000000u)!=0x14000000u','words[8]!=(0xd4200000u|(0xa0dfu<<5))');
 edit('    int64_t displacement=(int64_t)((words[8]&0x03ffffffu)^0x02000000u)-0x02000000;\n    uint64_t next=pc+4+(uint64_t)(displacement*4),sp=ss->__x[b];','    uint64_t next=target,sp=ss->__x[b];');
 edit('int klx_stack_resume(void *context)', 'int klx_stack_resume_far(void *context,uint64_t target)');
 edit('klj_x18_stack_resume(context)','klj_x18_stack_resume_far(context,target)');
 return text;
}
