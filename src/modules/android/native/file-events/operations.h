#ifndef MACLAUNCHER_FILE_EVENTS_H
#define MACLAUNCHER_FILE_EVENTS_H
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>
#include <sys/uio.h>
/* An operation snapshot owns no descriptor. It survives the actual close so
 * reuse of that descriptor cannot redirect the notification to another file. */
typedef struct {
    uint32_t mask;
    struct stat file, parent;
    char name[NAME_MAX + 1];
} klj_file_event;
int klj_file_event_interest(uint32_t mask);
klj_file_event klj_file_event_snapshot(int fd, uint32_t mask);
void klj_file_event_publish(const klj_file_event *event);
void klj_file_event_fd(int fd, uint32_t mask);
void klj_file_opened(int fd);
int klj_file_close(int fd);
int klj_file_fclose(FILE *file);
ssize_t klj_file_read(int fd,void *buffer,size_t size);
ssize_t klj_file_pread(int fd,void *buffer,size_t size,off_t offset);
ssize_t klj_file_readv(int fd,const struct iovec *vectors,int count);
size_t klj_file_fread(void *buffer,size_t size,size_t count,FILE *file);
int klj_file_fgetc(FILE *file);
char *klj_file_fgets(char *buffer,int size,FILE *file);
#endif
