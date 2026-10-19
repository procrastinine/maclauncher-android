#include "operations.h"
#include "../pollable-fds/bridge.h"
#include <sys/inotify.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

klj_file_event klj_file_event_snapshot(int fd, uint32_t mask) {
    klj_file_event event = {0};
    if (!klj_file_event_interest(mask)) return event;
    int saved = errno;
    if (fd >= 0 && fstat(fd, &event.file) == 0 &&
        (S_ISREG(event.file.st_mode) || S_ISDIR(event.file.st_mode))) {
        if (mask == IN_CLOSE) {
            int flags = fcntl(fd, F_GETFL);
            mask = flags < 0 ? 0 : ((flags & O_ACCMODE) == O_RDONLY ? IN_CLOSE_NOWRITE : IN_CLOSE_WRITE);
        }
        event.mask = mask;
        char path[PATH_MAX];
        if (fcntl(fd, F_GETPATH, path) == 0) {
            char *name = strrchr(path, '/');
            if (name && strlen(name+1) <= NAME_MAX) {
                strcpy(event.name, name+1);
                if (name == path) name[1] = 0; else *name = 0;
                if (stat(path, &event.parent)) event.name[0] = 0;
            }
        }
    }
    errno = saved;
    return event;
}
void klj_file_event_fd(int fd, uint32_t mask) {
    klj_file_event event = klj_file_event_snapshot(fd, mask);
    klj_file_event_publish(&event);
}
void klj_file_opened(int fd){
    klj_file_event_fd(fd,IN_OPEN);
}
int klj_file_close(int fd) {
    klj_file_event event = klj_file_event_snapshot(fd, IN_CLOSE);
    int result = klj_pollable_close(fd);
    if (result == 0) klj_file_event_publish(&event);
    return result;
}
static int close_stream(void *stream){return fclose(stream);}
int klj_file_fclose(FILE *file) {
    klj_file_event event = klj_file_event_snapshot(fileno(file), IN_CLOSE);
    int result = klj_descriptor_release(fileno(file),close_stream,file);
    if (result == 0) klj_file_event_publish(&event);
    return result;
}
ssize_t klj_file_read(int fd,void *buffer,size_t size){ssize_t result=klj_pollable_read(fd,buffer,size);if(result>0)klj_file_event_fd(fd,IN_ACCESS);return result;}
ssize_t klj_file_pread(int fd,void *buffer,size_t size,off_t offset){ssize_t result=klj_pollable_pread(fd,buffer,size,offset);if(result>0)klj_file_event_fd(fd,IN_ACCESS);return result;}
ssize_t klj_file_readv(int fd,const struct iovec *vectors,int count){ssize_t result=klj_pollable_readv(fd,vectors,count);if(result>0)klj_file_event_fd(fd,IN_ACCESS);return result;}
size_t klj_file_fread(void *buffer,size_t size,size_t count,FILE *file){size_t result=fread(buffer,size,count,file);if(result>0)klj_file_event_fd(fileno(file),IN_ACCESS);return result;}
int klj_file_fgetc(FILE *file){int result=fgetc(file);if(result!=EOF)klj_file_event_fd(fileno(file),IN_ACCESS);return result;}
char *klj_file_fgets(char *buffer,int size,FILE *file){char *result=fgets(buffer,size,file);if(result)klj_file_event_fd(fileno(file),IN_ACCESS);return result;}
