#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* The child uses only the original clone return and Linux exit syscall. Its
 * architectural x18 may point into ordinary guest memory, while the parent
 * restores its original caller value before returning to Bionic. */
extern pid_t fork_x18_probe(void *pointer);
__asm__(
".text\n"
".global fork_x18_probe\n"
".type fork_x18_probe,%function\n"
"fork_x18_probe:\n"
" stp x29,x30,[sp,#-32]!\n"
" stp x19,x20,[sp,#16]\n"
" mov x29,sp\n"
" mov x19,x18\n"
" mov x20,x0\n"
" cbz x0,1f\n"
" mov x18,x0\n"
"1:\n"
" mov x9,#0x1357\n"
" mov x10,#0x2468\n"
" stp x9,x10,[x18],#16\n"
" bl fork\n"
" cbnz x0,4f\n"
" cbz x20,2f\n"
" add x9,x20,#16\n"
" cmp x18,x9\n"
" b.ne 5f\n"
"2: ldp x9,x10,[x18,#-16]\n"
" mov x11,#0x1357\n"
" cmp x9,x11\n"
" b.ne 5f\n"
" mov x11,#0x2468\n"
" cmp x10,x11\n"
" b.ne 5f\n"
" str xzr,[x18,#-8]\n"
" mov x0,#0\n"
" b 3f\n"
"5: mov x0,#41\n"
"3: mov x8,#93\n"
" svc #0\n"
" brk #0\n"
"4: sub x18,x18,#16\n"
" mov x18,x19\n"
" ldp x19,x20,[sp,#16]\n"
" ldp x29,x30,[sp],#32\n"
" ret\n"
".size fork_x18_probe,.-fork_x18_probe\n");

static int check(void *pointer, const char *name) {
    pid_t child = fork_x18_probe(pointer);
    if (child < 0) { perror("fork"); return 1; }
    int status = 0;
    for (unsigned i = 0; i != 250; ++i) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                printf("PASS %s fork x18 and used bytes\n", name);
                return 0;
            }
            fprintf(stderr, "%s child status=%#x\n", name, status);
            return 1;
        }
        if (result < 0 && errno != EINTR) { perror("waitpid"); return 1; }
        usleep(20000);
    }
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    fprintf(stderr, "%s child exceeded bounded fork wait\n", name);
    return 1;
}

int main(void) {
    setbuf(stdout, NULL);
    if (check(NULL, "original SCS")) return 1;
    const size_t bytes = 256 * 1024;
    unsigned char *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { perror("mmap"); return 1; }
    void *pointer = memory + 128 * 1024;
    int result = check(pointer, "ordinary guest mapping");
    if (!result && ((uint64_t *)pointer)[1] != 0x2468) {
        fputs("fork child changed the parent's private mapping\n", stderr);
        result = 1;
    }
    if (munmap(memory, bytes)) { perror("munmap"); return 1; }
    if (!result) puts("PASS original Bionic fork keeps architectural x18 ownership");
    return result;
}
