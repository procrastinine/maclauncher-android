import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from './upstream/source-files.mjs';

// Keep Klepton's XR/image/ABI adapters. Android window presentation belongs to
// the selected driver's original Metal WSI, including its full swapchain API.
export function prepareVulkanSource(klepton, build) {
  let source = fs.readFileSync(path.join(klepton, 'runtime/gfx/kl_vulkan.c'), 'utf8');
  const replace = (before, after) => {
    if (source.split(before).length !== 2) throw Error('Pinned Vulkan boundary changed: ' + before.slice(0, 70));
    source = source.replace(before, after);
  };
  const block = (signature, replacement = '') => {
    const start=source.indexOf(signature);if(start<0)throw Error('Pinned Vulkan image boundary changed: '+signature);
    const open=source.indexOf('{',start);let depth=1,end=open+1;
    for(;depth&&end<source.length;end++){if(source[end]==='{')depth++;else if(source[end]==='}')depth--;}
    if(depth)throw Error('Unterminated Vulkan source block');
    source=source.slice(0,start)+replacement+source.slice(end);
  };
  // Preserve the selected driver's capabilities and the application's explicit
  // requests. A missing function is NULL, never a universal success function.
  block('static VKAPI_ATTR VkResult VKAPI_CALL klvk_missing_trap(void) {');
  block('static void klvk_note_missing(const char *name) {');
  source=source.replaceAll('    if (!fn) { klvk_note_missing(name); return (PFN_vkVoidFunction)klvk_missing_trap; }','');
  for(const kind of ['instance','device'])block('        if (have && !ext_supported(have, nhave, e)) {');
  source=source.replaceAll('    mine.enabledLayerCount = 0;\n    mine.ppEnabledLayerNames = NULL;','');
  replace('typedef struct { const char *names[64]; uint32_t n; } extlist;',
    'typedef struct { const char **names; uint32_t n; } extlist;');
  replace('    if (l->n >= 64) return;','');
  source=source.replaceAll('    extlist keep = {{0}, 0};',
    '    extlist keep = {calloc((size_t)(ci ? ci->enabledExtensionCount : 0) + 3, sizeof(char*)), 0};\n    if (!keep.names) {free(have);return VK_ERROR_OUT_OF_HOST_MEMORY;}');
  // Both creation calls consume extension names synchronously.
  replace('    VkResult r = real(&mine, alloc, out);','    VkResult r = real(&mine, alloc, out);\n    free(keep.names);');
  replace('    VkResult r = real(phys, &mine, alloc, out);','    VkResult r = real(phys, &mine, alloc, out);\n    free(keep.names);');
  // Tracking describes XR-owned resources; it cannot establish whether a
  // driver-provided image is valid. Remove the capture port's admission and
  // scratch-substitution layer, while retaining XR allocation/export metadata.
  block('static int klvk_img_adopt_mode(void) {');
  block('static int klvk_img_reg_ok(VkImage img) {');
  block('    if ((src && !klvk_img_reg_has(src)) || (dst && !klvk_img_reg_has(dst))) {');
  block('    if (ci && ci->image && !klvk_img_reg_ok(ci->image)) {');
  for(const signature of [
    '    if (KLVK_IMG_BAD(src) || KLVK_IMG_BAD(dst)) {',
    '    if (KLVK_IMG_BAD(img)) {'
  ])while(source.includes(signature))block(signature);
  block('static void VKAPI_CALL klvk_CmdPipelineBarrier(VkCommandBuffer cb,',
    `static void VKAPI_CALL klvk_CmdPipelineBarrier(VkCommandBuffer cb,
        VkPipelineStageFlags ss,VkPipelineStageFlags ds,VkDependencyFlags df,
        uint32_t mbc,const VkMemoryBarrier *mb,uint32_t bbc,const VkBufferMemoryBarrier *bb,
        uint32_t ibc,const VkImageMemoryBarrier *ib) {
    PFN_vkCmdPipelineBarrier real=(PFN_vkCmdPipelineBarrier)mvk_sym("vkCmdPipelineBarrier");
    if(real)real(cb,ss,ds,df,mbc,mb,bbc,bb,ibc,ib);
}`);
  replace('#define KLVK_IMG_BAD(img) ((img) && !klvk_img_reg_ok(img))','');
  block('static VkImage klvk_sub_wild(VkImage img) {','static VkImage klvk_sub_wild(VkImage img) {return img;}');
  replace('            VKT("instance ext %s -> ours (MoltenVK has no Android WSI)\\n", e);',
    '            ext_add(&keep, "VK_EXT_metal_surface");');
  replace('static VkResult klvk_CreateAndroidSurfaceKHR(', 'static VkResult klvk_CaptureAndroidSurfaceKHR(');
  replace('static void klvk_DestroySurfaceKHR(', 'static void klvk_DestroyCaptureSurfaceKHR(');
  replace('static const entry g_ours[] = {', fs.readFileSync(path.join(import.meta.dirname, 'vulkan-window.inc'), 'utf8') + '\nstatic const entry g_ours[] = {');
  replace('    E(GetPhysicalDeviceSurfaceSupportKHR), E(GetPhysicalDeviceSurfaceCapabilitiesKHR),\n    E(GetPhysicalDeviceSurfaceFormatsKHR), E(GetPhysicalDeviceSurfacePresentModesKHR),\n    E(CreateSwapchainKHR), E(DestroySwapchainKHR), E(GetSwapchainImagesKHR),\n    E(AcquireNextImageKHR), E(QueuePresentKHR),',
    '    // Original driver WSI owns all surface queries, swapchains and presentation.');
  const result = path.join(build, 'vulkan-window.c');writeChanged(result, source);return result;
}
