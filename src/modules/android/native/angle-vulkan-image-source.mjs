import fs from 'node:fs';
import path from 'node:path';

// Adopt explicitly enrolled Android VkImages. Ordinary external-image imports
// retain their original borrowed lifetime and public EGL behavior.
export function prepareAngleVulkanImageOwnership(source, output) {
  const patch = (file, changes) => {
    let text = fs.readFileSync(path.join(source, file), 'utf8');
    for (const [before, after] of changes) {
      if (text.split(before).length !== 2) throw Error('ANGLE Vulkan image source changed: ' + file);
      text = text.replace(before, after);
    }
    const target = path.join(output, file);
    fs.mkdirSync(path.dirname(target), {recursive: true});
    fs.writeFileSync(target, text);
  };
  const directory = 'src/libANGLE/renderer/vulkan/';
  patch(directory + 'VkImageImageSiblingVk.h', [
    ['    vk::ImageHelper *mImage = nullptr;', '    vk::ImageHelper *mImage = nullptr;\n    bool mHostOwned = false;'],
  ]);
  patch(directory + 'VkImageImageSiblingVk.cpp', [
    ['namespace rx\n{', `#include <mutex>
#include <map>

namespace
{
std::mutex gHostImageMutex;
std::map<std::pair<VkDevice, VkImage>, bool> gHostImages;

bool TakeHostImage(VkDevice device, VkImage image, bool *hostVisible)
{
    std::lock_guard<std::mutex> lock(gHostImageMutex);
    const auto found = gHostImages.find({device, image});
    if (found == gHostImages.end()) return false;
    *hostVisible = found->second;
    gHostImages.erase(found);
    return true;
}
}  // namespace

// Only pending ownership transfers live here. ANGLE's original image and
// renderer garbage queues retire the imported object after its last GPU use.
extern "C" __attribute__((visibility("default")))
bool kljAngleVkImageOwnership(VkDevice device, VkImage image, uint32_t transfer)
{
    if (device == VK_NULL_HANDLE || image == VK_NULL_HANDLE || transfer > 2)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(gHostImageMutex);
    if (transfer)
    {
        return gHostImages.emplace(std::make_pair(device, image), transfer == 1).second;
    }
    return gHostImages.erase({device, image}) != 0;
}

namespace rx
{`],
    ['    mImage->init2DWeakReference(displayVk, mVkImage.release(), getSize(), false, intendedFormatID,',
      '    const VkImage hostImage = mVkImage.getHandle();\n    mImage->init2DWeakReference(displayVk, mVkImage.release(), getSize(), false, intendedFormatID,'],
    ['    return angle::Result::Continue;\n}\n\nvoid VkImageImageSiblingVk::onDestroy',
      '    bool hostVisible = false;\n    mHostOwned = TakeHostImage(renderer->getDevice(), hostImage, &hostVisible);\n    if (mHostOwned)\n    {\n        mImage->setHostVisibleImport(hostVisible);\n    }\n    return angle::Result::Continue;\n}\n\nvoid VkImageImageSiblingVk::onDestroy'],
    ['        mImage->resetImageWeakReference();', `        if (mHostOwned)
        {
            mImage->releaseImage(renderer);
        }
        else
        {
            mImage->resetImageWeakReference();
        }`],
  ]);
  patch(directory + 'vk_helpers.h', [
    ['    bool isForeignImage() const { return mIsForeignImage; }', `    bool isForeignImage() const { return mIsForeignImage || mAndroidImport; }
    // Android imports retain shared gralloc storage. Reuse the original
    // submission tracker to publish only images actually used by this context.
    void setHostVisibleImport(bool hostVisible)
    {
        mAndroidImport = true;
        mHostVisibleImport = hostVisible;
        mCurrentLayout = ImageLayout::ForeignAccess;
        setEntireContentDefined();
    }`],
    ['        return mCurrentDeviceQueueIndex == kForeignDeviceQueueIndex;', '        return mAndroidImport ? mCurrentLayout == ImageLayout::ForeignAccess\n                                  : mCurrentDeviceQueueIndex == kForeignDeviceQueueIndex;'],
    ['    bool mIsForeignImage;', '    bool mIsForeignImage;\n    bool mHostVisibleImport = false;\n    bool mAndroidImport = false;'],
  ]);
  patch(directory + 'vk_helpers.cpp', [
    ['    ASSERT(mIsForeignImage);\n\n    const ImageMemoryBarrierData barrierData', '    ASSERT(isForeignImage());\n\n    const ImageMemoryBarrierData barrierData'],
    ['    barrier.subresourceRange.layerCount = mLayerCount;\n\n    mCurrentLayout', `    barrier.subresourceRange.layerCount = mLayerCount;
    if (mAndroidImport)
    {
        // A Metal shared texture has no FOREIGN queue ownership. The exact
        // image barrier also makes MoltenVK publish texture writes to its
        // backing MTLBuffer before the original submission fence signals.
        barrier.dstAccessMask = mHostVisibleImport
                                    ? VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT
                                    : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }

    mCurrentLayout`],
    ['    mCurrentDeviceQueueIndex     = kForeignDeviceQueueIndex;', '    if (!mAndroidImport)\n    {\n        mCurrentDeviceQueueIndex = kForeignDeviceQueueIndex;\n    }'],
    ['    imageMemoryBarrier->srcAccessMask = transitionFrom.srcAccessMask;', `    imageMemoryBarrier->srcAccessMask = transitionFrom.srcAccessMask;
    if (mHostVisibleImport && mCurrentLayout == ImageLayout::ForeignAccess)
    {
        imageMemoryBarrier->srcAccessMask |= VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
    }`],
    ['        mCurrentEvent.valid() && mCurrentDeviceQueueIndex == newDeviceQueueIndex', '        mCurrentEvent.valid() && mCurrentDeviceQueueIndex == newDeviceQueueIndex &&\n                !(mAndroidImport && mCurrentLayout == ImageLayout::ForeignAccess)'],
    ['    if (mCurrentDeviceQueueIndex == kForeignDeviceQueueIndex)\n    {\n        context->onForeignImageUse(this);', '    if (mCurrentDeviceQueueIndex == kForeignDeviceQueueIndex || mAndroidImport)\n    {\n        context->onForeignImageUse(this);'],
    ['    const bool hasQueueChange = mCurrentDeviceQueueIndex != context->getDeviceQueueIndex();', `    if (mAndroidImport)
    {
        context->onForeignImageUse(this);
        if (mCurrentLayout == ImageLayout::ForeignAccess)
        {
            barrierType = BarrierType::Pipeline;
        }
    }
    const bool hasQueueChange = mCurrentDeviceQueueIndex != context->getDeviceQueueIndex();`],
    ['    mUse.reset();\n    mImageSerial', '    mUse.reset();\n    mHostVisibleImport = false;\n    mAndroidImport = false;\n    mImageSerial'],
    ['void ImageHelper::resetImageWeakReference()\n{', 'void ImageHelper::resetImageWeakReference()\n{\n    mHostVisibleImport = false;\n    mAndroidImport = false;'],
  ]);
  // The original tracker appends these barriers to the same command batch and
  // resource serial as GL work, covering normal EGL fences and final GL refs.
  patch(directory + 'CommandQueue.cpp', [
    ['        state.primaryCommands.pipelineBarrier(\n            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,', `        VkPipelineStageFlags destinationStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        for (const auto &barrier : imagesToTransitionToForeign)
        {
            if (barrier.dstAccessMask & (VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT))
            {
                destinationStages |= VK_PIPELINE_STAGE_HOST_BIT;
            }
            else if (barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED)
            {
                destinationStages |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            }
        }
        state.primaryCommands.pipelineBarrier(
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, destinationStages,`],
  ]);
  patch(directory + 'vk_renderer.cpp', [
    ['void Renderer::enableDeviceExtensionsNotPromoted(const vk::ExtensionNameList &deviceExtensionNames)\n{', `void Renderer::enableDeviceExtensionsNotPromoted(const vk::ExtensionNameList &deviceExtensionNames)
{
#if defined(ANGLE_PLATFORM_APPLE)
    if (ExtensionFound("VK_EXT_metal_objects", deviceExtensionNames))
    {
        mEnabledDeviceExtensions.push_back("VK_EXT_metal_objects");
    }
#endif`],
  ]);
  patch(directory + 'FramebufferVk.cpp', [
    ['            mState.getColorAttachments()[colorIndexGL].isExternalImageWithoutIndividualSync();', `            mState.getColorAttachments()[colorIndexGL].isExternalImageWithoutIndividualSync() ||
            renderTarget->getImageForWrite().isForeignImage();`],
  ]);
}
