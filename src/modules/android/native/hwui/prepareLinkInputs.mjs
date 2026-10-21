import fs from'node:fs';import path from'node:path';import crypto from'node:crypto';

export async function prepareLinkInputs(ctx) {
const root=ctx.work,imageRoot=ctx.image.root;
const needed=JSON.parse(fs.readFileSync(new URL('./shared-libraries.json',import.meta.url))).needed;
const output=path.join(root,'module/link/shared');fs.mkdirSync(output,{recursive:true});
function guestFile(name){let file=path.join(imageRoot,'system/lib64',name),hops=0;while(fs.lstatSync(file).isSymbolicLink()){
 if(++hops>20)throw Error('Recursive original image alias '+name);const value=fs.readlinkSync(file);file=path.isAbsolute(value)?path.join(imageRoot,value):path.resolve(path.dirname(file),value);if(!file.startsWith(imageRoot+path.sep))throw Error('Escaped original image alias '+name);
 }if(!fs.statSync(file).isFile())throw Error('Original image dependency is not a regular file '+name);return file;}
const files=[];for(const name of needed){const file=guestFile(name),dest=path.join(output,name);if(fs.existsSync(dest)||fs.lstatSync(path.dirname(dest)).isFile()){try{fs.unlinkSync(dest)}catch(e){if(e.code!=='ENOENT')throw e;}}
 fs.symlinkSync(file,dest);const bytes=fs.readFileSync(file);files.push({name,source:file,size:bytes.length,sha256:crypto.createHash('sha256').update(bytes).digest('hex')});}
fs.writeFileSync(path.join(root,'module/link/shared-inputs.json'),JSON.stringify({imageRoot,imageContentSha256:ctx.image.contentSha256,files},null,2));console.log(JSON.stringify({originalSharedLibraries:files.length,output,bytes:files.reduce((s,r)=>s+r.size,0),copiedBytes:0}));

}
