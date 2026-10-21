import fs from'node:fs';import path from'node:path';import{execFileSync}from'node:child_process';

export async function generateReleaseFlags(ctx) {
const root=ctx.work,s=path.join(root,'module/source'),tool=ctx.tools.aconfig;
const orders=['root','ap2a','ap3a','ap4a','bp1a','bp2a','user'];
const packages=[
 ['hwui','com.android.graphics.hwui.flags',['framework/libs/hwui/aconfig/hwui_flags.aconfig']],
 ['surfaceflinger','com.android.graphics.surfaceflinger.flags',['native/services/surfaceflinger/surfaceflinger_flags.aconfig','native/services/surfaceflinger/surfaceflinger_flags_new.aconfig']],
 ['text','com.android.text.flags',['framework/core/java/android/text/flags/24Q3.aconfig','framework/core/java/android/text/flags/flags.aconfig']],
 ['accessibility','android.view.accessibility',['framework/core/java/android/view/accessibility/flags/accessibility_flags.aconfig']],
 ['egl','com.android.graphics.egl.flags',['native/opengl/libs/EGL/egl_flags.aconfig']],
 ['gui','com.android.graphics.libgui.flags',['native/libs/gui/libgui_flags.aconfig']],
];const results=[];
for(const [name,pkg,declarations]of packages){const dest=path.join(root,'module/gen/aconfig',name),cache=path.join(dest,'source-release.pb');fs.mkdirSync(dest,{recursive:true});
 const values=orders.flatMap(tier=>{const p=path.join(s,'release/aconfig',tier,pkg);return fs.existsSync(p)?fs.readdirSync(p).filter(f=>f.endsWith('.textproto')).sort().map(f=>path.join(p,f)):[]});
 const flags=['create-cache','--package',pkg,'--container','system','--default-permission','read_only',...declarations.flatMap(f=>['--declarations',path.join(s,f)]),...values.flatMap(p=>['--values',p]),'--cache',cache];
 execFileSync(tool,flags,{stdio:'pipe'});execFileSync(tool,['create-cpp-lib','--cache',cache,'--out',dest,'--mode','production']);
 results.push({name,pkg,declarations,defaultPermissionSource:'release/flag_values/ap2a/RELEASE_ACONFIG_FLAG_DEFAULT_PERMISSION.textproto',orders,values,headers:fs.readdirSync(path.join(dest,'include')),cpp:fs.readdirSync(dest).filter(f=>f.endsWith('.cc'))});
}
fs.writeFileSync(path.join(root,'release-flags-generation.json'),JSON.stringify({tool,releaseRevision:'5da94f17b7a143d754a3b250c08bff210d3b046b',results},null,2));console.log(results.map(({name,headers})=>({name,headers})));

}
