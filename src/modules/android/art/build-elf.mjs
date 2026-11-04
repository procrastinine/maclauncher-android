import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {writeChanged} from '../native/upstream/source-files.mjs';

function inputLink(file,target){
 target=fs.realpathSync(target);fs.mkdirSync(path.dirname(file),{recursive:true});
 const old=fs.lstatSync(file,{throwIfNoEntry:false});
 if(old){if(old.isSymbolicLink()&&fs.realpathSync(file)===target)return;throw Error('ART build input differs from the selected workspace: '+file);}
 fs.symlinkSync(target,file,'dir');
}

/** Invoke AIM's original pinned ART graph in the owned source projection.
 * Its source locks, ART patch series, Ninja graph and strip step remain the
 * producers; the target directory is a reusable compiler workspace. */
export function buildArtElfs({aim,fetched,sdk,work,jdk,python,jobs=4,cargo='cargo',env=process.env}){
 if([aim,fetched,sdk,work].some(value=>typeof value!=='string'||!path.isAbsolute(value)))throw Error('ART ELF builds require absolute aim, fetched, sdk and work paths.');
 aim=fs.realpathSync(aim);
 let environment={...process.env,...env,ANDROID_SDK_ROOT:fs.realpathSync(sdk)};
 if(jdk){
  if(!path.isAbsolute(jdk)||!fs.existsSync(path.join(jdk,'bin/javac')))throw Error('ART jdk requires an absolute JDK home.');
  environment.JAVA_HOME=fs.realpathSync(jdk);
 }
 if(python){if(!path.isAbsolute(python))throw Error('ART python requires an absolute executable.');environment.PATH=path.dirname(python)+path.delimiter+(environment.PATH||'');}
 work=path.resolve(work);fs.mkdirSync(work,{recursive:true});
 inputLink(path.join(aim,'_build'),fetched);inputLink(path.join(aim,'target'),work);
 if(!Number.isInteger(jobs)||jobs<1)throw Error('ART jobs requires a positive integer.');
 const n2file=path.join(aim,'crates/aim-build/src/n2db.rs'),n2=fs.readFileSync(n2file,'utf8'),n2marker='.arg0(ARGV0)\n            .arg("-C")',n2limited='.arg0(ARGV0)\n            .arg("-j")\n            .arg("'+jobs+'")\n            .arg("-C")';
 if(n2.includes(n2marker))writeChanged(n2file,n2.replace(n2marker,n2limited));else if(!n2.includes(n2limited))throw Error('Original ART compiler job boundary changed.');
 const image=fs.realpathSync(path.join(fetched,'android16-image.dmg')),mount=image.replace(/\.dmg$/,''),wasMounted=fs.existsSync(path.join(mount,'root.identity'));
 try{
 execFileSync(cargo,['run','--offline','--locked','--release','--quiet','--package','aim-build','--bin','cargo-aim','--','build','art'],{
  cwd:aim,env:{...environment,CARGO_BUILD_JOBS:String(jobs),CARGO_TARGET_DIR:work},stdio:'inherit'});
 }finally{if(!wasMounted&&fs.existsSync(path.join(mount,'root.identity')))execFileSync('hdiutil',['detach',mount],{stdio:'inherit'});}
 const art=path.join(work,'aim/art/stripped');
 for(const file of ['bin/dex2oat64',...['libartbase','libdexfile','libprofile','libart','libopenjdkjvm','libadbconnection','libopenjdkjvmti'].map(name=>'lib64/'+name+'.so')])
  if(!fs.statSync(path.join(art,file)).isFile())throw Error('Original ART producer did not produce '+file);
 return {art};
}
