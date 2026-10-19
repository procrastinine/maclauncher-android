#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

// HotSpot calls Darwin JNI. Original Android ELF entry points use AAPCS64,
// whose spilled primitive arguments occupy eight-byte slots. The registers
// agree; only the spilled arguments need repacking. Immutable signed text
// pages are remapped with adjacent writable records, as with Darwin libffi
// closures. More pages are allocated on demand; no JIT or RWX memory is used.
typedef struct {uint16_t offset;uint8_t size;uint8_t reserved;} StackSlot;
typedef struct {void *target;uint16_t stackBytes;uint16_t count;uint32_t reserved;StackSlot slots[256];void *(*normalize)(void *);} NativeABI;
_Static_assert(offsetof(NativeABI,slots)==16,"assembly record layout");
_Static_assert(sizeof(StackSlot)==4,"assembly slot layout");
_Static_assert(offsetof(NativeABI,normalize)==1040,"assembly environment normalizer layout");
enum { PAGE_BYTES=16384, ENTRY_BYTES=16, ENTRIES=PAGE_BYTES/ENTRY_BYTES };
extern const unsigned char klj_native_abi_page[];
extern void klj_native_abi_dispatch(void);
extern void klj_native_env_dispatch(void);
typedef struct Page {struct Page *next;mach_vm_address_t address;size_t used;NativeABI *records[ENTRIES];} Page;
static Page *pages;
static Page *new_page(char *error,size_t capacity){
    Page *page=calloc(1,sizeof(*page));
    if(!page){snprintf(error,capacity,"Android JNI stack adapter allocation failed");return NULL;}
    mach_vm_address_t address=0;
    kern_return_t status=mach_vm_allocate(mach_task_self(),&address,2*PAGE_BYTES,VM_FLAGS_ANYWHERE);
    if(status==KERN_SUCCESS){
        mach_vm_address_t text=address+PAGE_BYTES;vm_prot_t current=0,maximum=0;
        status=mach_vm_remap(mach_task_self(),&text,PAGE_BYTES,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,mach_task_self(),(mach_vm_address_t)klj_native_abi_page,FALSE,&current,&maximum,VM_INHERIT_SHARE);
        if(status==KERN_SUCCESS)status=mach_vm_protect(mach_task_self(),text,PAGE_BYTES,FALSE,VM_PROT_READ|VM_PROT_EXECUTE);
    }
    if(status!=KERN_SUCCESS){if(address)mach_vm_deallocate(mach_task_self(),address,2*PAGE_BYTES);free(page);snprintf(error,capacity,"Android JNI stack adapter mapping failed (%d)",status);return NULL;}
    page->address=address;page->next=pages;pages=page;return page;
}
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;

static void *publish(const NativeABI *record,void *dispatch,char *error,size_t capacity){
    pthread_mutex_lock(&mutex);
    for(Page *page=pages;page;page=page->next)for(size_t i=0;i<page->used;i++)if(!memcmp(page->records[i],record,sizeof *record)){void *entry=(void*)(page->address+PAGE_BYTES+i*ENTRY_BYTES);pthread_mutex_unlock(&mutex);return entry;}
    NativeABI *copy=malloc(sizeof *record);if(!copy){pthread_mutex_unlock(&mutex);snprintf(error,capacity,"Android JNI stack adapter allocation failed");return NULL;}
    Page *page=pages;if(!page||page->used==ENTRIES)page=new_page(error,capacity);
    if(!page){free(copy);pthread_mutex_unlock(&mutex);return NULL;}
    *copy=*record;size_t index=page->used++;
    page->records[index]=copy;
    void **data=(void**)(page->address+index*ENTRY_BYTES);data[0]=copy;data[1]=dispatch;
    void *entry=(void*)(page->address+PAGE_BYTES+index*ENTRY_BYTES);pthread_mutex_unlock(&mutex);return entry;
}

void *klj_native_env_abi(void *target,void *(*normalize)(void *),unsigned argument,char *error,size_t capacity){
    if(!target||!normalize||argument>=8){snprintf(error,capacity,"Invalid native environment ABI descriptor");return NULL;}
    NativeABI record={.target=target,.reserved=argument,.normalize=normalize};
    return publish(&record,(void*)klj_native_env_dispatch,error,capacity);
}

static const char *type(const char *s,int *size,int *floating){
    *floating=0;
    switch(*s++){
        case 'Z':case 'B':*size=1;break;
        case 'C':case 'S':*size=2;break;
        case 'I':*size=4;break;
        case 'J':*size=8;break;
        case 'F':*size=4;*floating=1;break;
        case 'D':*size=8;*floating=1;break;
        case 'L':{const char *end=strchr(s,';');if(!end)return NULL;s=end+1;*size=8;break;}
        case '[':{while(*s=='[')s++;int ignored,fp;s=type(s,&ignored,&fp);if(!s)return NULL;*size=8;break;}
        default:return NULL;
    }
    return s;
}

void *klj_native_abi(void *target,const char *signature,char *error,size_t capacity){
    NativeABI record={.target=target};unsigned integer=2,floating=0,offset=0;
    const char *s=signature;if(!s||*s++!='(')goto invalid;
    while(*s&&*s!=')'){
        int size,fp;s=type(s,&size,&fp);if(!s)goto invalid;
        unsigned *registers=fp?&floating:&integer;
        if(*registers<8){(*registers)++;continue;}
        if(record.count>=256)goto invalid;
        offset=(offset+size-1)&~(unsigned)(size-1);
        record.slots[record.count++]=(StackSlot){.offset=(uint16_t)offset,.size=(uint8_t)size};
        offset+=size;
    }
    if(*s!=')'||!s[1])goto invalid;

    record.stackBytes=(uint16_t)((record.count*8+15)&~15u);
    return publish(&record,(void*)klj_native_abi_dispatch,error,capacity);
invalid:
    snprintf(error,capacity,"Invalid Android JNI descriptor: %s",signature?signature:"(null)");return NULL;
}
