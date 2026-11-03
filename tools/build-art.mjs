import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {prepareAimSource} from '../src/modules/android/art/source.mjs';
import {buildArt} from '../src/modules/android/art/build.mjs';
import {buildArtElfs} from '../src/modules/android/art/build-elf.mjs';
import {exportArtSourceArchive} from '../src/modules/android/art/source-archive.mjs';

/** Build the ART supplier and its corresponding sources from explicit inputs. */
export async function buildArtRuntime({inputs,check=false}){
 if(check)return {id:'android-art',action:'check'};
 const {sources,runner={},sourceArchive:providedArchive,sourceCheckouts={},hwuiSources,nativeBridgeSources,buildWork,artBuild={},...options}=JSON.parse(fs.readFileSync(inputs));
 if(!path.isAbsolute(options.output||'')||!sources||['source','klepton','glesHeaders'].some(key=>!path.isAbsolute(sources[key]||'')))throw Error('ART inputs require absolute output, source, klepton and glesHeaders paths.');
 if(!options.host||!options.java||!options.inputs||!Array.isArray(options.bootClasspath))throw Error('ART builds require host, java, inputs and bootClasspath producer groups.');
 if(fs.existsSync(path.join(options.output,'runtime')))throw Error('ART supplier output already exists.');
 if(providedArchive!==undefined&&(!path.isAbsolute(providedArchive)||!fs.statSync(providedArchive).isFile()))throw Error('ART sourceArchive must be an absolute source archive path.');
 const toolPath=value=>typeof value==='string'?value:value?.path;
 const hwui=hwuiSources||(options.hwui?{sources:options.hwui.sources,registry:path.join(options.hwui.work||path.join(options.output,'build/hwui'),'module/sources/SOURCES.json'),registryCache:options.hwui.noticeInputs?.cargo?.registryCache||path.join(toolPath(options.hwui.tools?.cargoHome)||'','registry/cache')}:undefined);
 const bridge=options.nativeBridge;
 let ndkHeaders=bridge?.ndkHeaders||sources.ndkHeaders;
 if(!ndkHeaders&&bridge?.imports?.directory){const input=path.join(bridge.imports.directory,'guest-input.json');if(fs.existsSync(input))ndkHeaders=JSON.parse(fs.readFileSync(input)).include;}
 const bridgeSources=nativeBridgeSources||((bridge||options.inputs?.nativeBridge)?{ndkHeaders,arm32Sources:bridge?.arm32?.sources}:undefined);
 const checkouts={...(bridge?.bionicSource?{'aosp-bionic':bridge.bionicSource}:{}),...(bridge?.artSource?{'aosp-art':bridge.artSource}:{}),...(bridge?.ffi?.source?{libffi:bridge.ffi.source}:{}),...(bridge?.arm32?.source?{dynarmic:bridge.arm32.source}:{}),...sourceCheckouts};
 if(!providedArchive&&options.inputs?.hwuiLibrary&&!hwui)throw Error('ART HWUI source export requires its original sources and registry inputs.');
 const exportSources=()=>exportArtSourceArchive({output:path.join(options.output,'source.tar.gz'),sources,unicorn:runner.unicorn,sourceCheckouts:checkouts,hwuiSources:hwui,nativeBridgeSources:bridgeSources});
 // Newly compiled HWUI supplies its active Cargo identities through the normal
 // component source output. All other declared source inputs can be exported first.
 const afterBuild=!providedArchive&&options.hwui&&typeof hwui?.registry==='string'&&!fs.existsSync(hwui.registry);
 let sourceArchive=providedArchive||(!afterBuild&&exportSources());
 const workspace=buildWork||path.join(options.output,'build');
 if(!path.isAbsolute(workspace))throw Error('ART buildWork requires an absolute path.');
 const aim=path.join(workspace,'aim-source');
 prepareAimSource({...sources,output:aim,mutableCode:true});
 if(!options.inputs?.art){
  const fetched=artBuild.fetched||(sources.aosp&&path.dirname(sources.aosp));
  const produced=buildArtElfs({...artBuild,aim,fetched,work:artBuild.work||path.join(workspace,'art-target')});
  options.inputs={...options.inputs,art:produced.art};
 }
 const supplier=await buildArt({...options,host:{klepton:sources.klepton,...options.host,aim},runner:{unicorn:path.resolve(import.meta.dirname,'../src/external/unicorn'),...runner,aim}});
 if(afterBuild){
  try{sourceArchive=exportSources();}
  catch(error){
   // This invocation created the unpublished supplier. Keep reusable compiler
   // outputs while allowing the same source build to finish on a retry.
   fs.rmSync(path.join(options.output,'runtime'),{recursive:true,force:true});throw error;
  }
 }
 return {...supplier,root:path.join(options.output,'runtime'),sourceArchive};
}

export async function main(args){
 if(args.length===1&&args[0]==='--check')return buildArtRuntime({check:true});
 if(args.length!==2||args[0]!=='--inputs'||!path.isAbsolute(args[1]))throw Error('Use --check or --inputs with an absolute JSON path.');
 return buildArtRuntime({inputs:args[1]});
}
if(process.argv[1]&&pathToFileURL(path.resolve(process.argv[1])).href===import.meta.url)
 main(process.argv.slice(2)).then(value=>console.log(JSON.stringify(value))).catch(error=>{console.error(error.message);process.exitCode=1;});
