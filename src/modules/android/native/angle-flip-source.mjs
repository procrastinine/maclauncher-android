import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from './upstream/source-files.mjs';

// ANGLE's BSD-licensed Metal framebuffer-state implementation, as present at
// 37c976ffba9d2940add977cad9945b23de14c150 (angle-flip-source.json). Preserve
// the pinned backend and its normal backbuffer orientation; apply the original
// framebuffer flip state only to a framebuffer whose caller requests it.
export function prepareAngleFramebufferFlip(source,output){
  const directory='src/libANGLE/renderer/metal';
  const patch=(name,change)=>{
    const original=path.join(source,directory,name),target=path.join(output,directory,name);
    const text=fs.readFileSync(fs.existsSync(target)?target:original,'utf8');
    writeChanged(target,change(text));
  };
  const replace=(text,before,after,name)=>{
    if(text.split(before).length!==2)throw Error('ANGLE framebuffer orientation boundary changed: '+name);
    return text.replace(before,after);
  };
  patch('DisplayMtl.mm',text=>replace(text,'    mNativeExtensions = gl::Extensions();',
    '    mNativeExtensions = gl::Extensions();\n    mNativeExtensions.framebufferFlipYMESA = true;', 'Metal extension'));
  patch('FrameBufferMtl.h',text=>replace(text,'    bool flipY() const { return mFlipY; }',
    '    bool flipY() const { return mFlipY != mState.getFlipY(); }', 'backbuffer and framebuffer origin'));
  patch('FrameBufferMtl.mm',text=>{
    if(text.split('if (mFlipY)').length!==3)throw Error('ANGLE Metal framebuffer origin readers changed');
    text=text.replaceAll('if (mFlipY)','if (flipY())');
    text=replace(text,'    clearOpts.flipY          = mFlipY;', '    clearOpts.flipY          = flipY();','clear origin');
    return replace(text,'            case gl::Framebuffer::DIRTY_BIT_DRAW_BUFFERS:',
      '            case gl::Framebuffer::DIRTY_BIT_FLIP_Y:\n            case gl::Framebuffer::DIRTY_BIT_DRAW_BUFFERS:', 'viewport, scissor and winding invalidation');
  });
}
