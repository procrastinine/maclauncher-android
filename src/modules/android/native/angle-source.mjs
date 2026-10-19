import fs from 'node:fs';
import path from 'node:path';
import {adaptAngleMetalSync, adaptAngleMetalSyncHeader} from './angle-sync-source.mjs';
import {prepareAngleDithering} from './angle-dither-source.mjs';
import {copyChanged} from './upstream/source-files.mjs';

// Keep ANGLE's framebuffer implementation. Apple GPUs provide real B5G6R5
// render targets; expose that format for the IOSurface window owner as well.
export function prepareAngleRgb565(source, output) {
  prepareAngleDithering(source, output);
  const directory='src/libANGLE/renderer/metal';
  const patch=(name,changes)=>{
    let text=fs.readFileSync(path.join(source,directory,name),'utf8');
    for(const [before,after] of changes){
      if(text.split(before).length!==2)throw Error('ANGLE RGB565 source changed: '+name);
      text=text.replace(before,after);
    }
    const target=path.join(output,directory,name);fs.mkdirSync(path.dirname(target),{recursive:true});fs.writeFileSync(target,text);
  };
  patch('DisplayMtl.mm',[
    ['    mNativeExtensions.EGLImageExternalOES = false;', '    mNativeExtensions.EGLImageExternalOES = true;'],
    ['    mNativeExtensions.EGLImageExternalEssl3OES = false;', '    mNativeExtensions.EGLImageExternalEssl3OES = true;'],
    ['    for (int samples : samplesSupported)\n    {',`    for (GLenum colorFormat : {GL_RGBA8, GL_RGB565})
    {
        const bool rgb565 = colorFormat == GL_RGB565;
        if (rgb565 && getPixelFormat(angle::FormatID::R5G6B5_UNORM).actualFormatId !=
                          angle::FormatID::R5G6B5_UNORM)
        {
            continue;
        }
        config.renderTargetFormat = colorFormat;
        // CAMetalLayer is BGRA8; the host presents RGB565 IOSurfaces separately.
        config.surfaceType = rgb565 ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
        config.bindToTextureRGB = rgb565 ? EGL_TRUE : EGL_FALSE;
        config.bindToTextureRGBA = rgb565 ? EGL_FALSE : EGL_TRUE;
        for (int samples : samplesSupported)
        {`],
    ['        config.redSize    = 8;\n        config.greenSize  = 8;\n        config.blueSize   = 8;\n        config.alphaSize  = 8;',
     '        config.redSize    = rgb565 ? 5 : 8;\n        config.greenSize  = rgb565 ? 6 : 8;\n        config.blueSize   = rgb565 ? 5 : 8;\n        config.alphaSize  = rgb565 ? 0 : 8;'],
    ['        configs.add(config);\n    }\n\n    return configs;', '        configs.add(config);\n        }\n    }\n\n    return configs;'],
  ]);
  patch('SurfaceMtl.mm',[[
    '    mColorFormat = display->getPixelFormat(angle::FormatID::B8G8R8A8_UNORM);',
    '    mColorFormat = display->getPixelFormat(state.config->renderTargetFormat == GL_RGB565\n        ? angle::FormatID::R5G6B5_UNORM : angle::FormatID::B8G8R8A8_UNORM);',
  ]]);
  patch('IOSurfaceSurfaceMtl.mm',[
    ['constexpr std::array<IOSurfaceFormatInfo, 9>', 'constexpr std::array<IOSurfaceFormatInfo, 10>'],
    ['    {GL_RGB,         GL_UNSIGNED_BYTE,                  4, angle::FormatID::NONE},',
     '    {GL_RGB,         GL_UNSIGNED_BYTE,                  4, angle::FormatID::NONE},\n    {GL_RGB,         GL_UNSIGNED_SHORT_5_6_5,           2, angle::FormatID::R5G6B5_UNORM},'],
    ['    if (kIOSurfaceFormats[mIOSurfaceFormatIdx].internalFormat == GL_RGB)',
     '    if (kIOSurfaceFormats[mIOSurfaceFormatIdx].internalFormat == GL_RGB &&\n        kIOSurfaceFormats[mIOSurfaceFormatIdx].type == GL_UNSIGNED_BYTE)'],
  ]);
  // Imported RGBX images retain their RGB pixels, initialize only the emulated
  // alpha channel, and keep later framebuffer writes from changing it. The
  // ordinary texture allocation path already applies this format contract.
  const imageAlpha=texture=>`
    if (mFormat.intendedAngleFormat().alphaBits == 0 &&
        mFormat.actualAngleFormat().alphaBits != 0)
    {
        ANGLE_TRY(mtl::InitializeTextureContentsGPU(
            context, ${texture}, mFormat,
            mtl::ImageNativeIndex::FromBaseZeroGLIndex(gl::ImageIndex::Make2D(0)),
            MTLColorWriteMaskAlpha));
        ${texture}->setColorWritableMask(MTLColorWriteMaskAll & (~MTLColorWriteMaskAlpha));
    }
`;
  patch('TextureMtl.mm',[[
    '    mFormat = contextMtl->getPixelFormat(angleFormatId);\n\n    mSlices = mNativeTextureStorage->cubeFacesOrArrayLength();',
    '    mFormat = contextMtl->getPixelFormat(angleFormatId);\n'+imageAlpha('imageMtl->getTexture()')+'\n    mSlices = mNativeTextureStorage->cubeFacesOrArrayLength();',
  ],[
    '    if (type != imageMtl->getImageTextureType())',
    '    if (type != imageMtl->getImageTextureType() &&\n        !(type == gl::TextureType::External && imageMtl->getImageTextureType() == gl::TextureType::_2D))',
  ]]);
  patch('RenderbufferMtl.mm',[[
    '    mRenderTarget.set(mTexture, mtl::kZeroNativeMipLevel, 0, mFormat);',
    imageAlpha('mTexture')+'\n    mRenderTarget.set(mTexture, mtl::kZeroNativeMipLevel, 0, mFormat);',
  ]]);
  patch('mtl_utils.mm',[[
    '        case gl::TextureType::_2D:\n            return MTLTextureType2D;',
    '        case gl::TextureType::_2D:\n        case gl::TextureType::External:\n            return MTLTextureType2D;',
  ]]);
  // These imported images already contain RGB pixels in the device texture.
  // Keep the original external-sampler validation and operations; Metal uses
  // its ordinary 2D sampling representation for this storage format.
  patch('../../../compiler/translator/msl/SymbolEnv.cpp',[
    ['        // 2d textures\n        case EbtSampler2D:', '        // 2d textures\n        case EbtSamplerExternalOES:\n        case EbtSampler2D:'],
    ['        case EbtSamplerExternalOES:       // Only valid if OES_EGL_image_external exists:\n',''],
  ]);
  patch('../../../compiler/translator/msl/ProgramPrelude.cpp',
    ['texelFetch','texture','textureProj'].map(name=>{
      const prefix=`    putBuiltIn("${name}", [](ProgramPrelude &pp, const TFunction &func) {`+
        (name==='texelFetch'?'':'\n        const bool bias = func.getParamCount() == 3;')+
        '\n        switch (func.getParam(0)->getType().getBasicType())\n        {\n            case EbtSampler2D:';
      return [prefix,prefix.replace('            case EbtSampler2D:',
        '            case EbtSamplerExternalOES:\n            case EbtSampler2D:')];
    }));
  fs.writeFileSync(path.join(output,directory,'SyncMtl.mm'),adaptAngleMetalSync(fs.readFileSync(path.join(source,directory,'SyncMtl.mm'),'utf8')));
  fs.writeFileSync(path.join(output,directory,'SyncMtl.h'),adaptAngleMetalSyncHeader(fs.readFileSync(path.join(source,directory,'SyncMtl.h'),'utf8')));
  // Keep ANGLE's native build-time internal shader compilation. Xcode's Metal
  // tools can live in a separately installed toolchain; xcrun resolves that
  // owner without requiring metallib beside the selected C++ compiler.
  patch('BUILD.gn',[
    ['      mac_bin_path + "metal",','      "/usr/bin/xcrun",\n      "metal",'],
    ['      mac_bin_path + "metallib",','      "/usr/bin/xcrun",\n      "metallib",'],
  ]);
  copyChanged(path.join(source,directory,'metal_backend.gni'),path.join(output,directory,'metal_backend.gni'));
}
