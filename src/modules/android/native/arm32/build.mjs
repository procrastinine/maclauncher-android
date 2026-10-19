import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {generateImports,generateLoaderAbi,refreshDescriptors} from './imports-generate.mjs';
import {generateSignalAbi} from './signal-abi.mjs';
import {prepareElf32} from './elf-source.mjs';
import {buildCompilerRuntime} from './compiler-rt.mjs';
import {prepareFormatPlan} from './format-plan.mjs';
import {prepareSyscalls} from './syscall-sources.mjs';
import {nativePathFlags,nativeFlagsString} from '../upstream/native-paths.mjs';
import {components,arm32BuildProducts} from './components.mjs';

const root=path.dirname(fileURLToPath(import.meta.url));
const pin=JSON.parse(fs.readFileSync(path.join(root,'source.json')));
export async function buildArm32({source,output,jdk,ndkHeaders,python,bionicSource,elfLinker,compilerRuntime,ownerHeaders=[],importsDirectory,graphicsRegistry}) {
  const revision=execFileSync('git',['-C',source,'rev-parse','HEAD'],{encoding:'utf8'}).trim();
  if(revision!==pin.revision)throw Error('Dynarmic revision does not match source.json');
  if(!bionicSource)throw Error('ARM32 image representation requires the pinned Bionic source');
  fs.mkdirSync(output,{recursive:true});
  const elf=prepareElf32({bionicSource,output});
  const formatDirectory=path.join(output,'format');fs.mkdirSync(formatDirectory,{recursive:true});
  const format=prepareFormatPlan({bionicSource,output:formatDirectory});
  ownerHeaders=[...ownerHeaders,...elf.ownerHeaders,format.header];
  if(ndkHeaders) {
    if(!python||!jdk)throw Error('ARM32 native declarations require Python and the selected JDK');
    importsDirectory=path.join(output,'imports');
    await generateImports({include:ndkHeaders,python,output:importsDirectory,ownerHeaders,graphicsRegistry});
  }
  const syscallDirectory=path.join(output,'syscalls');
  let syscalls;
  if(importsDirectory){
    if(!python)throw Error('Original ARM32 syscall declarations require Python');
    const include=ndkHeaders||JSON.parse(fs.readFileSync(path.join(importsDirectory,'guest-input.json'),'utf8')).include;
    if(!ndkHeaders){refreshDescriptors({output:importsDirectory,include,python,graphicsRegistry});generateLoaderAbi({include,output:importsDirectory});generateSignalAbi({include,output:importsDirectory});}
    syscalls=prepareSyscalls({bionicSource,include,python,importsDirectory,output:syscallDirectory});
  }
  const run=(command,args)=>execFileSync(command,args,{stdio:'inherit'});
  const remap=nativeFlagsString(nativePathFlags({source,build:output,roots:[[bionicSource,'/src/bionic'],...(ndkHeaders?[[ndkHeaders,'/src/ndk']]:[]),...(jdk?[[jdk,'/toolchain/jdk']]:[])]}));
  run('cmake',['-S',root,'-B',output,'-DDYNARMIC_SOURCE='+source,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_OSX_ARCHITECTURES=arm64','-DJDK_ROOT='+(jdk||''),'-DARM32_IMPORTS_DIRECTORY='+(importsDirectory||''),'-DARM32_FORMAT_DIRECTORY='+formatDirectory,'-DARM32_SYSCALL_DIRECTORY='+syscallDirectory,
    ...['C','CXX','ASM'].map(language=>'-DCMAKE_'+language+'_FLAGS='+[process.env[language==='CXX'?'CXXFLAGS':language==='ASM'?'ASMFLAGS':'CFLAGS'],remap].filter(Boolean).join(' '))]);
  run('cmake',['--build',output,'--parallel','4']);
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});
  fs.cpSync(path.join(formatDirectory,'notices'),notices,{recursive:true});
  if(syscalls)fs.cpSync(syscalls.notices,notices,{recursive:true});
  let builtins;
  if(importsDirectory){
    if(!elfLinker)throw Error('ARM32 compiler runtime requires an ELF linker');
    const include=ndkHeaders||JSON.parse(fs.readFileSync(path.join(importsDirectory,'guest-input.json'),'utf8')).include;
    const archive=compilerRuntime||path.resolve(include,'../libclang_rt.builtins-arm-android.a');
    builtins=buildCompilerRuntime({archive,notice:path.join(path.dirname(archive),'NOTICE.toolchain'),linker:elfLinker,aeabi:elf.aeabi,allocation:elf.allocation,portableFamilies:elf.portableFamilies,include,output:path.join(output,'compiler-runtime')});
    fs.cpSync(builtins.notices,notices,{recursive:true});
  }
  if(importsDirectory&&fs.existsSync(path.join(importsDirectory,'notices')))
    fs.cpSync(path.join(importsDirectory,'notices'),notices,{recursive:true});
  if(ndkHeaders)fs.copyFileSync(path.join(root,'ndk-headers.json'),path.join(notices,'ndk-headers.json'));
  for(const [file,name]of[['LICENSE.txt','dynarmic.txt'],['README.md','dynarmic-dependencies.md'],['externals/fmt/LICENSE.rst','fmt.txt'],['externals/mcl/LICENSE','mcl.txt'],['externals/oaknut/LICENSE','oaknut.txt'],['externals/robin-map/LICENSE','robin-map.txt']])
    fs.copyFileSync(path.join(source,file),path.join(notices,name));
  const identity={schemaVersion:1,...pin,library:components.bindings.jvm,jniIntegration:!!jdk,nativeIntegration:!!importsDirectory};
  fs.writeFileSync(path.join(output,'arm32-build.json'),JSON.stringify(identity,null,2)+'\n');
  fs.writeFileSync(path.join(notices,'source.json'),JSON.stringify(identity,null,2)+'\n');
  return {...arm32BuildProducts(output),builtins:builtins?.library,identity,importsDirectory};
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url)) {
  const args=process.argv.slice(2),options={};
  for(let i=0;i<args.length;i+=2){
    if(!['--source','--output','--java-home','--imports-directory','--ndk-headers','--python','--bionic-source','--owner-header','--elf-linker','--compiler-runtime'].includes(args[i])||!args[i+1])throw Error('Expected --option path pairs');
    options[args[i].slice(2)]=path.resolve(args[i+1]);
  }
  if(!options.source||!options.output)throw Error('Expected --source and --output paths');
  await buildArm32({source:options.source,output:options.output,jdk:options['java-home'],importsDirectory:options['imports-directory'],ndkHeaders:options['ndk-headers'],python:options.python,bionicSource:options['bionic-source'],elfLinker:options['elf-linker'],compilerRuntime:options['compiler-runtime'],ownerHeaders:options['owner-header']?[options['owner-header']]:[]});
}
