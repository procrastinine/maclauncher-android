import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
// A package's licence texts live in <root>/licenses/, each distinct text once; licenses/components.txt names
// the text each component uses. Texts compare by a fingerprint of their bytes without whitespace, so the same
// licence in another layout is not stored again. A missing or unreadable source is skipped with a warning.
const fingerprint=bytes=>crypto.createHash('sha256').update(bytes.filter(byte=>byte>32)).digest('hex'),stored=new Map();
const warning=(...values)=>{try{console.log("::warning::",...values);}catch{}};
export function addLicense(root,component,source,licence=component){
 try{
  const dir=path.join(root,'licenses'),index=path.join(dir,'components.txt'),text=fs.readFileSync(source),key=fingerprint(text);
  fs.mkdirSync(dir,{recursive:true});
  if(!stored.has(dir)||!fs.existsSync(index)){ // a new or recreated folder
   fs.appendFileSync(index,'');
   stored.set(dir,new Map(fs.readdirSync(dir,{withFileTypes:true}).filter(entry=>entry.isFile()&&entry.name!=='components.txt').map(entry=>[fingerprint(fs.readFileSync(path.join(dir,entry.name))),entry.name])));
  }
  const texts=stored.get(dir),base=String(licence).replace(/[^\w.+-]+/g,'-');
  let file=texts.get(key);
  if(!file){
   file=base+'.txt';
   for(let n=2;fs.existsSync(path.join(dir,file));n++)file=`${base}-${n}.txt`;
   fs.writeFileSync(path.join(dir,file),text);texts.set(key,file);
  }
  fs.appendFileSync(index,`${component}: ${file}\n`);
 }catch(error){warning("No licence text for",component,error);}
}

/** Merge a producer's component index through the same package-level operation. */
export function includeLicenses(root,directory){
 try{if(path.resolve(root,'licenses')===path.resolve(directory))return;
 for(const row of fs.readFileSync(path.join(directory,'components.txt'),'utf8').split('\n').filter(Boolean)){
  const split=row.lastIndexOf(': '),file=row.slice(split+2);
  addLicense(root,row.slice(0,split),path.join(directory,file),file.replace(/\.txt$/,''));
 }}catch(error){warning("No licence texts from",directory,error);}
}

/** Add the licence texts at the top of a source directory, including the files of a LICENSES folder. */
export function addLicenses(root,component,directory){
 try{
  const names=fs.readdirSync(directory).filter(name=>/^(licen[cs]e|copying|notice|copyright)/i.test(name));
  for(const name of names){
   const file=path.join(directory,name);
   let inner=[];try{if(fs.statSync(file).isDirectory())inner=fs.readdirSync(file);}catch{}
   if(inner.length)for(const each of inner)addLicense(root,component,path.join(file,each));
   else addLicense(root,component,file);
  }
 }catch(error){warning("No licence texts in",directory,error);}
}
