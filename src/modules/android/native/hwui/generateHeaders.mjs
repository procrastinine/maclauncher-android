import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {parseBlueprint} from '../upstream/blueprint.mjs';
export function generateHeaders(ctx) {
 const s=ctx.source,g=ctx.gen;
 for(const dir of ['include','src','public'])fs.mkdirSync(path.join(g,dir),{recursive:true});
 const run=(tool,args)=>execFileSync(tool,args,{encoding:'utf8',maxBuffer:16e6});
 run(ctx.tools.syspropCpp,['--header-dir',path.join(g,'include'),'--source-dir',path.join(g,'src'),'--include-name','HWUIProperties.sysprop.h','--public-header-dir',path.join(g,'public'),path.join(s,'framework/libs/hwui/HWUIProperties.sysprop')]);
 for(const [flag,file] of [['--header','include/statslog_hwui.h'],['--cpp','src/statslog_hwui.cpp']]){
  run(ctx.tools.statsLogApiGen,[flag,path.join(g,file),'--module','hwui','--namespace','android,uirenderer,stats',...(flag==='--cpp'?['--importHeader','statslog_hwui.h']:[])]);
 }
 run(ctx.tools.protoc,['--proto_path='+path.join(s,'framework/libs/hwui'),'--cpp_out='+path.join(g,'src'),path.join(s,'framework/libs/hwui/protos/graphicsstats.proto')]);
 const packages=['android.hidl.base@1.0','android.hidl.token@1.0','android.hidl.manager@1.0','android.hardware.graphics.common@1.0','android.hardware.graphics.common@1.1','android.hardware.graphics.common@1.2','android.hardware.media@1.0','android.hardware.graphics.bufferqueue@1.0','android.hardware.graphics.bufferqueue@2.0'];
 for(const pkg of packages)run(ctx.tools.hidlGen,['-Lc++-headers','-o',path.join(g,'include'),'-R','-randroid.hardware:'+path.join(s,'hardware-interfaces'),'-randroid.hidl:'+path.join(s,'hidl/transport'),pkg]);
 const gui=path.join(s,'native/libs/gui'),input=path.join(s,'native/libs/input');
 const modules=dir=>parseBlueprint(fs.readFileSync(path.join(dir,'Android.bp'),'utf8'),{file:path.join(dir,'Android.bp'),parser:ctx.tools.blueprintParser}).modules;
 const sourceGroups=[[gui,'android_gui_aidl'],[gui,'guiconstants_aidl'],[input,'inputconstants_aidl']];
 const output=path.join(ctx.components,'native-gui');fs.mkdirSync(path.join(output,'include'),{recursive:true});fs.mkdirSync(path.join(output,'src'),{recursive:true});
 for(const [dir,name] of sourceGroups){const group=modules(dir).find(m=>m.properties.name===name);if(!group)throw Error('Original HWUI interface group is absent: '+name);for(const pattern of group.properties.srcs){for(const file of /[*?[]/.test(pattern)?fs.globSync(pattern,{cwd:dir}):[pattern])run(ctx.tools.aidl,['--lang=cpp','-I'+gui,'-I'+path.join(gui,'aidl'),'-I'+path.join(s,'native/libs/binder/aidl'),'-I'+input,'-h',path.join(output,'include'),'-o',path.join(output,'src'),path.join(dir,file)]);}}
 return {packages,sourceGroups:sourceGroups.map(([,name])=>name)};
}
