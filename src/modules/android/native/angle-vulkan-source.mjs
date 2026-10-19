import fs from 'node:fs';
import path from 'node:path';
import {prepareAngleVulkanImageOwnership} from './angle-vulkan-image-source.mjs';
import {writeChanged} from './upstream/source-files.mjs';

// ANGLE keeps the Vulkan renderer and loader ABI. The runtime's ordinary
// managed-driver selection owns the physical library, just as for native
// Android Vulkan callers; no host-wide library search or second driver owner.
export function prepareAngleVulkan(source,output) {
  prepareAngleVulkanImageOwnership(source,output);
  const file='src/common/vulkan/libvulkan_loader.cpp';
  let text=fs.readFileSync(path.join(source,file),'utf8');
  const before='void *OpenLibVulkan()\n{';
  if(text.split(before).length!==2)throw Error('ANGLE Vulkan loader boundary changed');
  text=text.replace(before,`${before}
#if defined(ANGLE_PLATFORM_APPLE)
    const std::string selected = GetEnvironmentVar("KL_MVK_DYLIB");
    if (!selected.empty())
    {
        return OpenSystemLibraryWithExtension(selected.c_str(), SearchType::SystemDir);
    }
#endif
`);
  writeChanged(path.join(output,file),text);
  // An Android native window already supplies the driver's retained
  // CAMetalLayer carrier. Reuse it rather than inserting an unregistered
  // child layer; ordinary CALayer callers keep ANGLE's original ownership.
  const window='src/libANGLE/renderer/vulkan/mac/WindowSurfaceVkMac.mm';
  let surface=fs.readFileSync(path.join(source,window),'utf8');
  const first='    mMetalLayer        = [[CAMetalLayer alloc] init];';
  const last='    [layer addSublayer:mMetalLayer];';
  const begin=surface.indexOf(first),end=surface.indexOf(last,begin)+last.length;
  if(begin<0||end<last.length||surface.indexOf(first,begin+1)!==-1)throw Error('ANGLE Vulkan native layer boundary changed');
  const original=surface.slice(begin,end);
  surface=surface.slice(0,begin)+`    if ([layer isKindOfClass:[CAMetalLayer class]])
    {
        mMetalLayer = [static_cast<CAMetalLayer *>(layer) retain];
        mMetalLayer.device = mMetalDevice;
        mMetalLayer.framebufferOnly = NO;
    }
    else
    {
${original.split('\n').map(line=>'    '+line).join('\n')}
    }`+surface.slice(end);
  writeChanged(path.join(output,window),surface);
}
