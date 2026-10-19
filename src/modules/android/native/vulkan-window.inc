// Native Android window leases are supplied by the existing graphics owner.
// Vulkan handles, presentation timing and synchronization remain driver-owned.
typedef int (*klj_vk_acquire)(void *,void **,void **);
typedef void (*klj_vk_release)(void *);
static klj_vk_acquire klj_vk_window_acquire;
static klj_vk_release klj_vk_window_release;
typedef struct klj_vk_surface {
    VkSurfaceKHR surface;
    void *owner;
    struct klj_vk_surface *next;
} klj_vk_surface;
static klj_vk_surface *klj_vk_surfaces;

// Capability discovery uses a private original-driver instance. It neither
// populates the application's instance/device tables nor consumes a swapchain.
#include "vulkan-capabilities.h"
static pthread_once_t klj_vk_version_once=PTHREAD_ONCE_INIT;
static KljVulkanCapabilities klj_vk_capabilities={0,-1};
static void klj_vk_measure_version(void){
    if(kl_vulkan_available())klj_vk_capabilities=klj_vulkan_query_capabilities(real_gipa);
}
unsigned klj_vulkan_api_version(void){pthread_once(&klj_vk_version_once,klj_vk_measure_version);return klj_vk_capabilities.version;}
int klj_vulkan_api_level(void){pthread_once(&klj_vk_version_once,klj_vk_measure_version);return klj_vk_capabilities.level;}

int klj_vulkan_set_window_bridge(klj_vk_acquire acquire,klj_vk_release release) {
    if(!acquire||!release)return 0;
    pthread_mutex_lock(&g_lock);
    int same=!klj_vk_window_acquire||(klj_vk_window_acquire==acquire&&klj_vk_window_release==release);
    if(same){klj_vk_window_acquire=acquire;klj_vk_window_release=release;}
    pthread_mutex_unlock(&g_lock);return same;
}
static VkResult klvk_CreateAndroidSurfaceKHR(VkInstance instance,
        const VkAndroidSurfaceCreateInfoKHR *info,const VkAllocationCallbacks *allocator,VkSurfaceKHR *surface) {
    if(!info||!surface){VKI("vkCreateAndroidSurfaceKHR failed: invalid create info or output (%d)\n",VK_ERROR_INITIALIZATION_FAILED);return VK_ERROR_INITIALIZATION_FAILED;}
    if(!klj_vk_window_acquire){VKI("vkCreateAndroidSurfaceKHR failed: native window bridge is unavailable (%d)\n",VK_ERROR_INITIALIZATION_FAILED);return VK_ERROR_INITIALIZATION_FAILED;}
    PFN_vkCreateMetalSurfaceEXT create=(PFN_vkCreateMetalSurfaceEXT)real_gipa(instance,"vkCreateMetalSurfaceEXT");
    if(!create){VKI("vkCreateAndroidSurfaceKHR failed: driver has no vkCreateMetalSurfaceEXT (%d)\n",VK_ERROR_EXTENSION_NOT_PRESENT);return VK_ERROR_EXTENSION_NOT_PRESENT;}
    klj_vk_surface *entry=calloc(1,sizeof(*entry));if(!entry){VKI("vkCreateAndroidSurfaceKHR failed: surface allocation (%d)\n",VK_ERROR_OUT_OF_HOST_MEMORY);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    void *layer=NULL;
    if(!klj_vk_window_acquire(info->window,&entry->owner,&layer)){VKI("vkCreateAndroidSurfaceKHR failed: native window %p has no available presentation layer (%d)\n",(void*)info->window,VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);free(entry);return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;}
    VkMetalSurfaceCreateInfoEXT native={.sType=VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
        .pNext=info->pNext,.flags=info->flags,.pLayer=layer};
    VkResult result=create(instance,&native,allocator,surface);
    if(result!=VK_SUCCESS){VKI("vkCreateAndroidSurfaceKHR failed: driver vkCreateMetalSurfaceEXT returned %d for native window %p\n",result,(void*)info->window);klj_vk_window_release(entry->owner);free(entry);return result;}
    entry->surface=*surface;
    pthread_mutex_lock(&g_lock);entry->next=klj_vk_surfaces;klj_vk_surfaces=entry;pthread_mutex_unlock(&g_lock);
    return result;
}
static void klvk_DestroySurfaceKHR(VkInstance instance,VkSurfaceKHR surface,const VkAllocationCallbacks *allocator) {
    if(!surface)return;
    PFN_vkDestroySurfaceKHR destroy=(PFN_vkDestroySurfaceKHR)real_gipa(instance,"vkDestroySurfaceKHR");
    if(destroy)destroy(instance,surface,allocator);
    pthread_mutex_lock(&g_lock);klj_vk_surface **link=&klj_vk_surfaces,*entry=NULL;
    while(*link){if((*link)->surface==surface){entry=*link;*link=entry->next;break;}link=&(*link)->next;}
    pthread_mutex_unlock(&g_lock);
    if(entry){klj_vk_window_release(entry->owner);free(entry);}
}
