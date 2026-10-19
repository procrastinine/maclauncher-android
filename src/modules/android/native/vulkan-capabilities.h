#pragma once
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include <string.h>

typedef struct {uint32_t version;int level;} KljVulkanCapabilities;

// Android's Vulkan hardware levels describe these original core features.
// Keep this rule shared by the JVM driver bridge and ART's Android loader.
// CTS: tests/tests/graphics/src/android/graphics/cts/VulkanFeaturesTest.java.
static inline int klj_vulkan_hardware_level(const VkPhysicalDeviceFeatures *f){
    if(!f->textureCompressionETC2)return -1;
    return f->fullDrawIndexUint32&&f->imageCubeArray&&f->independentBlend&&
        f->geometryShader&&f->tessellationShader&&f->sampleRateShading&&
        f->textureCompressionASTC_LDR&&f->fragmentStoresAndAtomics&&
        f->shaderImageGatherExtended&&f->shaderUniformBufferArrayDynamicIndexing&&
        f->shaderSampledImageArrayDynamicIndexing?1:0;
}
static inline uint32_t klj_vulkan_feature_version(uint32_t version){
    // PackageManager uses the Android feature encodings (1.0.3, 1.1.0, ...),
    // not the implementation's patch revision.
    if(version<VK_MAKE_VERSION(1,0,3))return 0;
    return version<VK_MAKE_VERSION(1,1,0)?VK_MAKE_VERSION(1,0,3):version&~0xfffu;
}
static inline KljVulkanCapabilities klj_vulkan_query_capabilities(PFN_vkGetInstanceProcAddr get){
    KljVulkanCapabilities result={0,-1};if(!get)return result;
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)get(NULL,"vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties extensions=(PFN_vkEnumerateInstanceExtensionProperties)get(NULL,"vkEnumerateInstanceExtensionProperties");
    PFN_vkEnumerateInstanceVersion version=(PFN_vkEnumerateInstanceVersion)get(NULL,"vkEnumerateInstanceVersion");
    if(!create||!extensions)return result;
    uint32_t loader_version=VK_API_VERSION_1_0;
    if(version&&version(&loader_version)!=VK_SUCCESS)return result;
    uint32_t count=0;if(extensions(NULL,&count,NULL)!=VK_SUCCESS)return result;
    VkExtensionProperties *available=calloc(count?count:1,sizeof(*available));if(!available)return result;
    VkResult status=extensions(NULL,&count,available);int portability=0;
    if(status==VK_SUCCESS)for(uint32_t i=0;i<count;i++)if(!strcmp(available[i].extensionName,"VK_KHR_portability_enumeration"))portability=1;
    free(available);if(status!=VK_SUCCESS)return result;
    const char *names[]={"VK_KHR_portability_enumeration"};
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Android graphics capabilities",.apiVersion=loader_version};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,
        .flags=portability?VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR:0,
        .enabledExtensionCount=portability?1u:0u,.ppEnabledExtensionNames=portability?names:NULL};
    VkInstance instance=VK_NULL_HANDLE;if(create(&info,NULL,&instance)!=VK_SUCCESS)return result;
    PFN_vkDestroyInstance destroy=(PFN_vkDestroyInstance)get(instance,"vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices devices=(PFN_vkEnumeratePhysicalDevices)get(instance,"vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties=(PFN_vkGetPhysicalDeviceProperties)get(instance,"vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFeatures features=(PFN_vkGetPhysicalDeviceFeatures)get(instance,"vkGetPhysicalDeviceFeatures");
    if(devices&&properties&&features&&devices(instance,&count,NULL)==VK_SUCCESS){
        VkPhysicalDevice *physical=calloc(count?count:1,sizeof(*physical));
        if(physical){
            if(devices(instance,&count,physical)==VK_SUCCESS){
                for(uint32_t i=0;i<count;i++){
                    VkPhysicalDeviceProperties p;VkPhysicalDeviceFeatures f;properties(physical[i],&p);features(physical[i],&f);
                    int level=klj_vulkan_hardware_level(&f);
                    // A loader's patch revision does not limit the physical
                    // device's patch within the same Vulkan major/minor API.
                    uint32_t loader_limit=loader_version|0xfffu;
                    uint32_t supported=klj_vulkan_feature_version(p.apiVersion<loader_limit?p.apiVersion:loader_limit);
                    // Never combine different devices' version and level.
                    if(level>=0&&supported&&(level>result.level||(level==result.level&&supported>result.version)))result=(KljVulkanCapabilities){supported,level};
                }
            }
            free(physical);
        }
    }
    if(destroy)destroy(instance,NULL);
    return result;
}
