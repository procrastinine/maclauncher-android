#include "tcg/tcg-apple-jit.h"

#define NATIVE_EXCLUSIVE_STORE_ATTEMPT 0x10000u

/* Native reservation operations behind QEMU's original A64 decoder. No guest
 * instruction is decoded here. Only the already-decoded size/register fields
 * cross this host memory boundary. A lost host reservation is a real failure. */
static void native_exclusive_clear(struct uc_struct *uc)
{
    __asm__ volatile("clrex" ::: "memory");
    uc->native_exclusive_active = 0;
    uc->native_exclusive_retry = 0;
}
#define NATIVE_EXCLUSIVE_ACCESS(access) \
    void *pointer = env->uc->native_atomic_owner(env->uc->native_atomic_owner_data, address, length, access); \
    if (!pointer) { \
        env->uc->invalid_addr = address; env->uc->invalid_error = (access & UC_PROT_WRITE) ? UC_ERR_WRITE_UNMAPPED : UC_ERR_READ_UNMAPPED; \
        native_exclusive_clear(env->uc); cpu_loop_exit_restore(env_cpu(env), return_pc); \
    } \
    env->uc->native_atomic_guest = address; env->uc->native_atomic_host = (uintptr_t)pointer; \
    env->uc->native_atomic_length = length; env->uc->native_atomic_retaddr = return_pc; \
    if (sigsetjmp(env->uc->native_atomic_jump, 0)) { \
        env->uc->native_atomic_active = 0; native_exclusive_clear(env->uc); \
        env->uc->invalid_error = (access & UC_PROT_WRITE) ? UC_ERR_WRITE_UNMAPPED : UC_ERR_READ_UNMAPPED; \
        cpu_loop_exit_restore(env_cpu(env), return_pc); \
    } \
    env->uc->native_atomic_active = 1

static uint64_t native_exclusive_endian(uint64_t value, unsigned size, bool big)
{
    if (!big || size == 0) return value;
    return size == 1 ? bswap16(value) : size == 2 ? bswap32(value) : bswap64(value);
}
void HELPER(native_exclusive_clear)(CPUARMState *env) { native_exclusive_clear(env->uc); }
void HELPER(native_exclusive_load)(CPUARMState *env, uint64_t address, uint32_t flags)
{
    const uintptr_t return_pc = GETPC();
    unsigned size = flags & 3, pair = (flags >> 2) & 1, big = (flags >> 3) & 1;
    unsigned rt = (flags >> 4) & 31, rt2 = (flags >> 9) & 31;
    size_t length = (size_t)1 << (size + pair);uint64_t low = 0, high = 0;
    NATIVE_EXCLUSIVE_ACCESS(UC_PROT_READ);
    env->uc->native_exclusive_guest = address; env->uc->native_exclusive_host = (uintptr_t)pointer;
    env->uc->native_exclusive_length = length; env->uc->native_exclusive_active = 1;env->uc->native_exclusive_retry = 0;
    if (pair && size == 3) __asm__ volatile("ldaxp %0, %1, [%2]" : "=&r"(low), "=&r"(high) : "r"(pointer) : "memory");
    else if (size + pair == 3) __asm__ volatile("ldaxr %0, [%1]" : "=&r"(low) : "r"(pointer) : "memory");
    else if (size == 2) __asm__ volatile("ldaxr %w0, [%1]" : "=&r"(low) : "r"(pointer) : "memory");
    else if (size == 1) __asm__ volatile("ldaxrh %w0, [%1]" : "=&r"(low) : "r"(pointer) : "memory");
    else __asm__ volatile("ldaxrb %w0, [%1]" : "=&r"(low) : "r"(pointer) : "memory");
    env->uc->native_atomic_active = 0;
    if (pair && size == 2) { high = low >> 32; low = (uint32_t)low; }
    if (rt != 31) env->xregs[rt] = native_exclusive_endian(low, size, big);
    if (pair && rt2 != 31) env->xregs[rt2] = native_exclusive_endian(high, size, big);
}
void HELPER(native_exclusive_store)(CPUARMState *env, uint64_t address, uint32_t flags)
{
    const uintptr_t return_pc = GETPC();
    unsigned size = flags & 3, pair = (flags >> 2) & 1, big = (flags >> 3) & 1;
    unsigned rt = (flags >> 4) & 31, rt2 = (flags >> 9) & 31, rd = (flags >> 14) & 31;
    size_t length = (size_t)1 << (size + pair);uint32_t status = 1;
    const bool reserved = env->uc->native_exclusive_active;
    uint64_t low = native_exclusive_endian(rt == 31 ? 0 : env->xregs[rt], size, big);
    uint64_t high = native_exclusive_endian(rt2 == 31 ? 0 : env->xregs[rt2], size, big);
    if (!env->uc->native_exclusive_active || env->uc->native_exclusive_guest != address || env->uc->native_exclusive_length != length) goto done;
    {
        /* The owner admits this attempt without publishing a write. */
        NATIVE_EXCLUSIVE_ACCESS(UC_PROT_READ | UC_PROT_WRITE | NATIVE_EXCLUSIVE_STORE_ATTEMPT);
        if ((uintptr_t)pointer != env->uc->native_exclusive_host) goto done;
        if (pair && size == 3) __asm__ volatile("stlxp %w0, %1, %2, [%3]" : "=&r"(status) : "r"(low), "r"(high), "r"(pointer) : "memory");
        else if (size + pair == 3) { if (pair) low = (uint32_t)low | ((uint64_t)(uint32_t)high << 32); __asm__ volatile("stlxr %w0, %1, [%2]" : "=&r"(status) : "r"(low), "r"(pointer) : "memory"); }
        else if (size == 2) __asm__ volatile("stlxr %w0, %w1, [%2]" : "=&r"(status) : "r"(low), "r"(pointer) : "memory");
        else if (size == 1) __asm__ volatile("stlxrh %w0, %w1, [%2]" : "=&r"(status) : "r"(low), "r"(pointer) : "memory");
        else __asm__ volatile("stlxrb %w0, %w1, [%2]" : "=&r"(status) : "r"(low), "r"(pointer) : "memory");
        env->uc->native_atomic_active = 0;
    }
done:
    env->uc->native_atomic_active = 0;
    jit_write_protect(env->uc->current_executable);
    native_exclusive_clear(env->uc);if (rd != 31) env->xregs[rd] = status;
    env->uc->native_exclusive_retry = reserved && status != 0;
    if (!status && env->uc->native_written)
        env->uc->native_written(env->uc->native_atomic_owner_data, address, length);
}
#undef NATIVE_EXCLUSIVE_ACCESS
#undef NATIVE_EXCLUSIVE_STORE_ATTEMPT

/* Called only after the original DC ZVA helper completes successfully. */
void HELPER(native_dc_zva_written)(CPUARMState *env, uint64_t address)
{
    const uint64_t length = (uint64_t)4 << env_archcpu(env)->dcz_blocksize;
    if (env->uc->native_written)
        env->uc->native_written(env->uc->native_atomic_owner_data, address & ~(length - 1), length);
}
