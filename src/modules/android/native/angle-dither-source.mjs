import fs from 'node:fs';
import path from 'node:path';

// Use ANGLE's existing low-bit framebuffer dithering in its Metal translator.
// The operation stays in the fragment shader and follows the application's
// GL_DITHER and blend state; the real framebuffer format remains unchanged.
export function prepareAngleDithering(source, output) {
  const patch = (file, replacements) => {
    let text = fs.readFileSync(path.join(source, file), 'utf8');
    for (const [before, after] of replacements) {
      if (text.split(before).length !== 2) throw Error('ANGLE dithering source changed: ' + file);
      text = text.replace(before, after);
    }
    const target = path.join(output, file);
    fs.mkdirSync(path.dirname(target), {recursive: true});
    fs.writeFileSync(target, text);
  };
  const ditherSources = '  "src/compiler/translator/tree_ops/spirv/EmulateDithering.cpp",\n  "src/compiler/translator/tree_ops/spirv/EmulateDithering.h",\n';
  patch('src/compiler.gni', [
    [ditherSources, ''],
    ['angle_translator_sources = [\n', 'angle_translator_sources = [\n' + ditherSources],
  ]);
  patch('src/compiler/translator/tree_ops/spirv/EmulateDithering.cpp', [
    ['if (type.getQualifier() == EvqFragmentOut)',
      'if (type.getQualifier() == EvqFragmentOut || type.getQualifier() == EvqFragColor ||\n            type.getQualifier() == EvqFragData)'],
    ['    // fragmentOutput.rgb += ditherValue', `    // Premultiplied source-over must add the same alpha-weighted noise as
    // straight source-over. Bits16..23 carry one flag per color attachment;
    // the existing low16bits retain the original upstream format controls.
    if (type.getNominalSize() == 4)
    {
        TIntermTyped *premultiplied = new TIntermBinary(
            EOpBitwiseAnd, ditherControl->deepCopy(), CreateUIntNode(1u << (16 + location)));
        TIntermTyped *condition = new TIntermBinary(EOpNotEqual, premultiplied, CreateUIntNode(0));
        TIntermSequence alphaArguments = {
            new TIntermSwizzle(fragmentOutput->deepCopy(), {3}),
            CreateFloatNode(0.0f, EbpMedium), CreateFloatNode(1.0f, EbpMedium)};
        TIntermTyped *alpha = CreateBuiltInFunctionCallNode("clamp", &alphaArguments, *symbolTable, 100);
        TIntermBlock *scale = new TIntermBlock;
        scale->appendStatement(new TIntermBinary(
            EOpMulAssign, ditherValue->deepCopy(),
            alpha));
        ditherBlock->appendStatement(new TIntermIfElse(condition, scale, nullptr));
    }

    // fragmentOutput.rgb += ditherValue`],
    ['TIntermTyped *ditherControl = specConst->getDither();',
      'TIntermTyped *ditherControl = specConst ? specConst->getDither() : nullptr;'],
  ]);
  patch('src/compiler/translator/msl/TranslatorMSL.cpp', [
    ['#include "compiler/translator/tree_ops/InitializeVariables.h"',
      '#include "compiler/translator/tree_ops/InitializeVariables.h"\n#include "compiler/translator/tree_ops/spirv/EmulateDithering.h"'],
    ['        bool usesSampleInterpolation = false;', `        // Reuse the same low-bit output algorithm as the Vulkan backend, with
        // the existing driver uniform selecting the actual Metal attachments.
        if (!EmulateDithering(this, compileOptions, root, &symbolTable, nullptr,
                             driverUniforms))
        {
            return false;
        }
        usesFragCoord = true;

        bool usesSampleInterpolation = false;`],
  ]);
  patch('src/libANGLE/renderer/metal/ContextMtl.h', [
    ['        uint32_t unused;', '        uint32_t dither;'],
  ]);
  patch('src/libANGLE/renderer/metal/ContextMtl.mm', [
    ['            case gl::state::DIRTY_BIT_DITHER_ENABLED:\n                break;',
      '            case gl::state::DIRTY_BIT_DITHER_ENABLED:\n                invalidateDriverUniforms();\n                break;'],
    ['                updateBlendDescArray(glState.getBlendStateExt());\n                break;',
      '                updateBlendDescArray(glState.getBlendStateExt());\n                invalidateDriverUniforms();\n                break;'],
    ['    mDriverUniforms.depthRange[0] = mState.getNearPlane();', `    mDriverUniforms.dither = 0;
    if (mState.isDitherEnabled())
    {
        for (size_t colorIndex : mDrawFramebuffer->getState().getEnabledDrawBuffers())
        {
            bool premultiplied = false;
            // Source-over in either straight or premultiplied form. Other
            // arithmetic blend modes retain ANGLE's existing behavior.
            if (mState.isBlendEnabledIndexed(static_cast<GLuint>(colorIndex)))
            {
                const auto &blend = mState.getBlendStateExt();
                const auto sourceFactor = blend.getSrcColorIndexed(colorIndex);
                if ((sourceFactor != gl::BlendFactorType::SrcAlpha &&
                     sourceFactor != gl::BlendFactorType::One) ||
                    blend.getDstColorIndexed(colorIndex) != gl::BlendFactorType::OneMinusSrcAlpha)
                {
                    continue;
                }
                if (sourceFactor == gl::BlendFactorType::One)
                {
                    premultiplied = true;
                }
            }
            uint32_t control = sh::vk::kDitherControlNoDither;
            switch (mRenderPipelineDesc.outputDescriptor.colorAttachments[colorIndex].pixelFormat)
            {
                case MTLPixelFormatABGR4Unorm:
                    control = sh::vk::kDitherControlDither4444;
                    break;
                case MTLPixelFormatBGR5A1Unorm:
                case MTLPixelFormatA1BGR5Unorm:
                    control = sh::vk::kDitherControlDither5551;
                    break;
                case MTLPixelFormatB5G6R5Unorm:
                    control = sh::vk::kDitherControlDither565;
                    break;
                default:
                    break;
            }
            if (control != sh::vk::kDitherControlNoDither && premultiplied)
            {
                mDriverUniforms.dither |= 1u << (16 + colorIndex);
            }
            mDriverUniforms.dither |= control << (2 * colorIndex);
        }
    }
    mDriverUniforms.depthRange[0] = mState.getNearPlane();`],
  ]);
}
