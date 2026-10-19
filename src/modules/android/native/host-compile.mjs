import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {spawnSync,execFileSync} from 'node:child_process';
import {writeChanged,copyChanged} from './upstream/source-files.mjs';
import {nativePathFlags,publishNativeBinary} from './upstream/native-paths.mjs';

const quote=value=>"'"+String(value).replaceAll("'","'\\''")+"'";
const ninjaPath=value=>value.replaceAll('$','$$').replaceAll(' ','$ ').replaceAll(':','$:');
const shell=values=>values.map(quote).join(' ').replaceAll('$','$$');
const digest=value=>crypto.createHash('sha256').update(value).digest('hex');

/** Clang supplies the complete compile/link job arguments. Ninja retains the
 * resulting objects and compiler depfiles; this code does not interpret flags
 * or maintain a second header/dependency model. */
function compileHostProduct({name,inputs,flags=[],extra=[],directory,output,cwd,ninja,python,compiler='clang',pathRoots=[]},kind){
  // Map declared source/include inputs. Unrelated runtime providers must not
  // change every translation unit's command when their version path changes.
  const arguments_=[...inputs,...flags,...extra];
  const usedRoots=pathRoots.filter(([root])=>arguments_.some(value=>String(value).includes(path.resolve(root))));
  const portable=nativePathFlags({source:cwd,build:path.dirname(path.resolve(directory)),roots:usedRoots});
  directory=path.resolve(directory,name);fs.mkdirSync(directory,{recursive:true});
  const cached=path.join(directory,name);
  const productFlags=kind==='library'?['-dynamiclib','-Wl,-install_name,@rpath/'+name]:kind==='object'?['-c']:[];
  const planned=spawnSync(compiler,[...flags,...portable,...productFlags,'-o',cached,...inputs,...extra,'-MD','-###'],{cwd,encoding:'utf8',maxBuffer:16*1024*1024});
  if(planned.error)throw planned.error;
  if(planned.status!==0)throw Error(planned.stderr||'Clang could not prepare the native build');
  // The driver prints shell-quoted argv. Python's standard shell lexer reads
  // those records without executing a shell or evaluating substitutions.
  const parser='import json,shlex,sys\nprint(json.dumps([shlex.split(line) for line in sys.stdin if line.lstrip().startswith(chr(34))]))\n';
  const commands=JSON.parse(execFileSync(python,['-c',parser],{input:planned.stderr,encoding:'utf8',maxBuffer:32*1024*1024}));
  if(!commands.length)throw Error('Clang did not produce native build jobs');
  const replacements=new Map(),objects=[],lines=['builddir = '+ninjaPath(directory),'rule compile','  command = $command','  description = Compile $in','  depfile = $out.d','  deps = gcc','rule assemble','  command = $command','  description = Assemble $in','rule link','  command = $command','  description = Link $out'];
  for(const original of commands){
    const command=original.map(value=>replacements.get(value)||value),position=command.indexOf('-o');
    if(position<0||!command[position+1])throw Error('Clang native build job has no output');
    const compile=command.includes('-cc1')||command.includes('-cc1as');
    if(compile){
      const assembler=command.includes('-cc1as'),source=command.at(-1),old=command[position+1];
      const key=digest(JSON.stringify([source,assembler?'assembler':command.includes('-E')?'preprocessor':'compiler']));
      const object=old===cached?cached:path.join(directory,key+(path.extname(old)||'.o'));command[position+1]=object;
      if(!assembler){const dependency=command.indexOf('-dependency-file');if(dependency>=0)command[dependency+1]=object+'.d';else command.splice(position,0,'-dependency-file',object+'.d');
        const target=command.indexOf('-MT');if(target>=0)command[target+1]=object;else command.push('-MT',object);}
      const dump=command.indexOf('-dumpdir');if(dump>=0)command[dump+1]=object+'-';
      replacements.set(old,object);if(assembler||command.includes('-emit-obj'))objects.push(object);
      lines.push('build '+ninjaPath(object)+': '+(assembler?'assemble':'compile')+' '+ninjaPath(source)+' | '+ninjaPath(command[0]),'  command = '+shell(command));
    }else{
      const dependencies=[...new Set(command.filter(value=>path.isAbsolute(value)&&fs.existsSync(value)&&fs.statSync(value).isFile()).concat(objects))].filter(file=>file!==cached);
      lines.push('build '+ninjaPath(command[position+1])+': link '+dependencies.map(ninjaPath).join(' '),'  command = '+shell(command));
    }
  }
  const file=path.join(directory,'build.ninja');writeChanged(file,lines.join('\n')+'\n');
  // Every generated output is inside this library's cache. Ninja removes
  // obsolete units through its own log; published runtimes are copied out.
  execFileSync(ninja,['-f',file,'-t','cleandead'],{cwd,stdio:['ignore','ignore','inherit']});
  execFileSync(ninja,['-f',file,'-k','0','--',cached],{cwd,stdio:'inherit'});
  if(output&&path.resolve(output)!==cached){
    if(kind==='object')copyChanged(cached,output);
    else publishNativeBinary(cached,output);
  }
  return {objects,file,output:cached};
}

export const compileHostLibrary=options=>compileHostProduct(options,'library');
export const compileHostObject=options=>compileHostProduct(options,'object');
export const compileHostExecutable=options=>compileHostProduct(options,'executable');
