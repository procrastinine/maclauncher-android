import {recordHwuiSources} from './source-records.mjs';
import {addLicenses} from '../upstream/licenses.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {prepareHwuiInputs} from './inputs.mjs';
import {prepareCommonPool} from './common-pool-source.mjs';
import {generateHeaders} from './generateHeaders.mjs';
import {generateGraphicsAidl} from './generateGraphicsAidl.mjs';
import {generateGuiAidl} from './generateGuiAidl.mjs';
import {generateReleaseFlags} from './generateReleaseFlags.mjs';
import {generatePerfetto} from './generatePerfetto.mjs';
import {buildCore} from './buildCore.mjs';
import {buildStatics} from './buildStatics.mjs';
import {buildAvif} from './buildAvif.mjs';
import {buildGenerated} from './buildGenerated.mjs';
import {buildPerfetto} from './buildPerfetto.mjs';
import {prepareLinkInputs} from './prepareLinkInputs.mjs';
import {linkHwui} from './linkHwui.mjs';
import {validateHwui} from './validateHwui.mjs';

/** Build the complete pinned API36 ARM64 libhwui against its original image ABI. */
export async function buildHwui(options) {
 const ctx=await prepareHwuiInputs(options);
 for(const name of ['clangxx','ninja','blueprintParser','aidl','aconfig','protoc','python','syspropCpp','statsLogApiGen','hidlGen','rustc','cargo','cargoHome','libclang'])if(!ctx.tools[name])throw Error('An explicit HWUI build tool is required: '+name);
 const sysroot=ctx.tools.sysroot||path.join(path.dirname(path.dirname(ctx.tools.clangxx)),'sysroot');
 const configFile=path.join(sysroot,'usr/include/c++/v1/__config_site');let config=fs.readFileSync(configFile,'utf8');
 if(config.split('#define _LIBCPP_ABI_NAMESPACE __ndk1').length!==2)throw Error('The declared NDK libc++ ABI configuration differs');
 config=config.replace('#define _LIBCPP_ABI_NAMESPACE __ndk1','#define _LIBCPP_ABI_NAMESPACE __1');fs.mkdirSync(path.join(ctx.gen,'libcxx'),{recursive:true});fs.writeFileSync(path.join(ctx.gen,'libcxx/__config_site'),config);
 const adapted=prepareCommonPool({framework:ctx.sources.framework,output:path.join(ctx.adapted,'framework/libs/hwui')});
 fs.mkdirSync(path.join(ctx.work,'adapted/thread'),{recursive:true});for(const name of ['CommonPool.h','CommonPool.cpp'])fs.copyFileSync(path.join(adapted.output,'thread',name),path.join(ctx.work,'adapted/thread',name));
 fs.copyFileSync(new URL('./perfetto-target.json',import.meta.url),path.join(ctx.work,'perfetto-component-inputs.json'));
 await generateGraphicsAidl(ctx);await generateGuiAidl(ctx);generateHeaders(ctx);await generateReleaseFlags(ctx);await generatePerfetto(ctx);
 await buildCore({...ctx,build:false});
 // Each target owns a separate output/depfile directory; all use the same
 // original class definition and explicit source/tool inputs.
 await Promise.all([buildCore(ctx),buildStatics(ctx),buildAvif(ctx),buildGenerated(ctx),buildPerfetto(ctx)]);
 await prepareLinkInputs(ctx);
 const product=validateHwui(ctx,await linkHwui(ctx));
 for(const [component,directory] of Object.entries(ctx.sources))addLicenses(ctx.module,component,directory);
 const sourceRecords=await recordHwuiSources(ctx,options.sourceInputs);
 return {...product,sourceRecords};
}
