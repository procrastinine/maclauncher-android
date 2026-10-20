#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void owner(const char *path,unsigned uid,unsigned gid,unsigned mode){
 struct stat info;assert(!stat(path,&info));
 if(info.st_uid!=uid||info.st_gid!=gid||(info.st_mode&07777)!=mode){fprintf(stderr,"%s: %u:%u %o expected %u:%u %o\n",path,info.st_uid,info.st_gid,info.st_mode&07777,uid,gid,mode);assert(0);}
}
static void readable(const char *path){int fd=open(path,O_RDONLY);assert(fd>=0);char byte;assert(read(fd,&byte,1)==1);struct stat s;assert(!fstat(fd,&s)&&s.st_uid==0&&s.st_gid==0);assert(!close(fd));}
int main(void){
 owner("/code",0,0,0755);owner("/code/nested",0,0,0755);
 owner("/code/nested/unseen",0,0,0644);owner("/code-alias/nested/unseen",0,0,0644);
 owner("/code/exec",0,0,0755);owner("/code/owner-exec",0,0,0755);owner("/code/explicit",10023,10024,0440);
 owner("/code/explicit-exec",0,0,0600);owner("/code/explicit-owner-exec",0,0,0744);owner("/code/partial",10023,0,0644);owner("/standalone",0,0,0644);owner("/standalone-exec",0,0,0755);
 owner("/code/original",10037,10038,0440);owner("/code/late/deep/file",0,0,0644);
 owner("/undeclared/file",0,0,0600);
 owner("/app",10074,10074,0700);owner("/app/private",10074,10074,0600);owner("/app/exec",10074,10074,0700);owner("/peer",10075,10075,0700);
 struct stat link;assert(!lstat("/code/private-link",&link)&&(link.st_mode&07777)==0777);
 assert(!setgid(10074));assert(!setuid(10074));
 readable("/code/nested/unseen");readable("/code-alias/nested/unseen");readable("/standalone");
 readable("/code/late/deep/file");
 assert(!access("/code/exec",X_OK));
 assert(!access("/code/owner-exec",X_OK));assert(!access("/code-alias/owner-exec",X_OK));assert(!access("/standalone-exec",X_OK));
 errno=0;assert(access("/code/nested/unseen",X_OK)<0&&errno==EACCES);
 errno=0;assert(access("/code/explicit-exec",X_OK)<0&&errno==EACCES);
 errno=0;assert(access("/code/explicit-owner-exec",X_OK)<0&&errno==EACCES);
 errno=0;assert(access("/code/owner-exec",W_OK)<0&&errno==EROFS);
 errno=0;assert(access("/standalone-exec",W_OK)<0&&errno==EROFS);
 assert(!access("/app/private",R_OK|W_OK));assert(!access("/app/exec",X_OK));
 errno=0;assert(open("/code/nested/unseen",O_WRONLY)<0&&errno==EROFS);
 errno=0;assert(open("/code/explicit",O_RDONLY)<0&&errno==EACCES);
 errno=0;assert(open("/code/original",O_RDONLY)<0&&errno==EACCES);
 errno=0;assert(open("/undeclared/file",O_RDONLY)<0&&errno==EACCES);
 assert(open("/code/private-link",O_RDONLY)<0);
 assert(open("/peer/private/secret",O_RDONLY)<0);
 puts("PASS declared readonly code: lazy modes, original bytes, descriptors, aliases, explicit metadata, symlinks and peer privacy");
 return 0;
}
