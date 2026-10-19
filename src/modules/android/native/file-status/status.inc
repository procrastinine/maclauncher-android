
/* Android's *at entry points have Linux flags and Bionic output records.
 * They must not fall through to Darwin's differently sized struct stat. */
int klj_fstatat(int directory, const char *name, void *output, int flags) {
    enum {LINUX_NOFOLLOW=0x100, LINUX_NO_AUTOMOUNT=0x800, LINUX_EMPTY_PATH=0x1000};
    if (flags & ~(LINUX_NOFOLLOW|LINUX_NO_AUTOMOUNT|LINUX_EMPTY_PATH)) {
        errno=EINVAL; return -1;
    }
    if (!name || !output) { errno=EFAULT; return -1; }
    int fd=directory==-100 ? AT_FDCWD : directory;
    if (!name[0] && (flags&LINUX_EMPTY_PATH))
        return fd==AT_FDCWD ? klb_stat(".",output) : klb_fstat(fd,output);
    if (fd==AT_FDCWD || name[0]=='/')
        return flags&LINUX_NOFOLLOW ? klb_lstat(name,output) : klb_stat(name,output);
    char mapped[PATH_MAX];
    const char *actual=kl_guest_path(name,mapped,sizeof(mapped));
    if (actual[0]=='/')
        return flags&LINUX_NOFOLLOW ? klb_lstat(actual,output) : klb_stat(actual,output);
    struct stat info;
    int result=fstatat(fd,actual,&info,(flags&LINUX_NOFOLLOW)?AT_SYMLINK_NOFOLLOW:0);
    if (!result) stat_to_bionic(&info,output);
    return result;
}
