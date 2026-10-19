/* Linux's raw entry returns the initialized mask extent; Bionic's public
 * wrapper returns zero and clears its remaining buffer. CPU availability is
 * derived from the same owner as sysconf and the process CPU files. */
extern long kl_cpu_count(void);
extern unsigned klj_library_word_bytes(void);
int klj_sched_getaffinity_kernel(int pid,size_t size,void *mask) {
    (void)pid;
    int saved_errno=errno;
    size_t cpus=(size_t)kl_cpu_count(),word=klj_library_word_bytes();
    size_t words=cpus/(word*8)+(cpus%(word*8)!=0);
    if(words>SIZE_MAX/word||size<words*word||size%word){errno=EINVAL;return -1;}
    size_t bytes=words*word;
    if(!mask){errno=EFAULT;return -1;}
    memset(mask,0,bytes);
    unsigned char *bits=mask;
    memset(bits,0xff,cpus/8);
    if(cpus%8)bits[cpus/8]=(unsigned char)((1u<<(cpus%8))-1u);
    errno=saved_errno;
    return (int)bytes;
}
int klb_sched_getaffinity(int pid,size_t size,void *mask) {
    int bytes=klj_sched_getaffinity_kernel(pid,size,mask);
    if(bytes<0)return -1;
    memset((unsigned char *)mask+bytes,0,size-(size_t)bytes);
    return 0;
}
