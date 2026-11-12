import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

const sha=/^[a-f0-9]{40}$/;
// Tokenize source without treating generated Rust/Java/JS strings or comments
// as imports. The source project uses literal import/require specifiers only.
export function moduleImports(text){
 const tokens=[];let i=0;
 function scan(stopBrace=false){let braces=0;
 while(i<text.length){
  const ch=text[i],next=text[i+1];
  if(stopBrace&&ch==='}'&&braces===0){i++;return;}
  if(/\s/.test(ch)){i++;continue;}
  if(ch==='/'&&next==='/'){i=text.indexOf('\n',i+2);if(i<0)break;continue;}
  if(ch==='/'&&next==='*'){const end=text.indexOf('*/',i+2);if(end<0)throw Error('Unterminated source comment');i=end+2;continue;}
  if(ch==='`'){
   i++;let closed=false;while(i<text.length){if(text[i]==='\\'){i+=2;continue;}if(text[i]==='`'){i++;closed=true;break;}if(text[i]==='$'&&text[i+1]==='{'){i+=2;scan(true);continue;}i++;}
   if(!closed)throw Error('Unterminated source template');tokens.push({type:'template',value:'`template`'});continue;
  }
  if(ch==='"'||ch==="'"){
   const start=++i;while(i<text.length){if(text[i]==='\\'){i+=2;continue;}if(text[i]===ch)break;i++;}
   if(i===text.length)throw Error('Unterminated source string');
   tokens.push({type:'string',value:text.slice(start,i)});i++;continue;
  }
  const previous=tokens.at(-1)?.value;
  if(ch==='/'&&(!previous||['=','(',',','[',':','!','?',';','{','return','=>','&&','||'].includes(previous))){
   i++;let characterClass=false;while(i<text.length){if(text[i]==='\\'){i+=2;continue;}if(text[i]==='[')characterClass=true;if(text[i]===']')characterClass=false;if(text[i]==='/'&&!characterClass){i++;break;}i++;}while(/[a-z]/i.test(text[i]||'')&&i<text.length)i++;tokens.push({type:'regex',value:'/regex/'});continue;
  }
  const word=text.slice(i).match(/^[A-Za-z_$][\w$]*/);
  if(word){tokens.push({type:'word',value:word[0]});i+=word[0].length;continue;}
  const operator=text.slice(i,i+2);if(['=>','&&','||'].includes(operator)){tokens.push({type:'punctuation',value:operator});i+=2;continue;}
  if(ch==='{')braces++;if(ch==='}')braces--;
  tokens.push({type:'punctuation',value:ch});i++;
 }
 }
 scan();
 const result=[];
 for(let i=0;i<tokens.length;i++){
  const token=tokens[i];if(token.type!=='word'||!['require','import','export'].includes(token.value))continue;
  if(tokens[i-1]?.value==='.')continue;
  if(tokens[i+1]?.value==='('&&tokens[i+2]?.type==='string'){result.push(tokens[i+2].value);continue;}
  if(token.value==='require'||tokens[i+1]?.value==='.'||tokens[i+1]?.value==='(')continue;
  if(token.value==='import'&&tokens[i+1]?.type==='string'){result.push(tokens[i+1].value);continue;}
  for(let j=i+1;j<tokens.length&&tokens[j].value!==';';j++)if(tokens[j].value==='from'&&tokens[j+1]?.type==='string'){result.push(tokens[j+1].value);break;}
 }
 return result;
}
export function relativeImports(file){
 try{return moduleImports(fs.readFileSync(file,'utf8')).filter(name=>name.startsWith('.'));}catch(error){throw Error(file+': '+error.message);}
}
export function resolveImport(file,name){
 const target=path.resolve(path.dirname(file),name);
 return [target,...['.js','.mjs','.cjs','.json'].map(suffix=>target+suffix),path.join(target,'index.js')].find(file=>fs.existsSync(file)&&fs.statSync(file).isFile());
}
function files(directory){
 return fs.readdirSync(directory,{withFileTypes:true}).sort((a,b)=>a.name.localeCompare(b.name)).flatMap(e=>{
  const name=path.join(directory,e.name);if(path.basename(directory)==='src'&&e.name==='external')return [];return e.isDirectory()?files(name):[name];
 });
}
export function checkSources(root,{metadataOnly=false}={}){
 root=path.resolve(root);const issues=[],projects=JSON.parse(fs.readFileSync(path.join(root,'upstreams.json'))).projects;
 const git=(args)=>execFileSync('git',['-C',root,...args],{encoding:'utf8',stdio:['ignore','pipe','pipe']}).trim();
 const config=git(['config','--null','--file','.gitmodules','--list']).split('\0').filter(Boolean).map(row=>row.split('\n'));
 const modules=new Map();
 for(const [key,value]of config){const match=key.match(/^submodule\.(.+)\.(path|url|maclauncher-revision|maclauncher-role)$/);if(match){const row=modules.get(match[1])||{};row[match[2]]=value;modules.set(match[1],row);}}
 const declared=new Map(),ids=new Set();let pins=0;
 for(const project of projects){
  if(ids.has(project.id)||declared.has(project.path)||!sha.test(project.revision)||!/^src\/external\/[A-Za-z0-9_./-]+$/.test(project.path)||project.path.split('/').includes('..')){issues.push('Invalid or duplicate supplier: '+project.id);continue;}
  ids.add(project.id);declared.set(project.path,project);
  const module=modules.get(project.id);
  if(!module||module.path!==project.path||module.url!==project.repository||module['maclauncher-revision']!==project.revision||module['maclauncher-role']!==(project.referenceOnly?'reference':'source'))issues.push('Git module metadata differs: '+project.id);
  if(!project.pinRefs?.length)issues.push('Missing source pin reference: '+project.id);
  for(const ref of project.pinRefs||[]){
   try{
    if(ref.kind!=='json'||!ref.path.startsWith('src/')||ref.path.split('/').includes('..'))throw Error('unsupported pin');
    const value=ref.property.split('.').reduce((value,key)=>value?.[key],JSON.parse(fs.readFileSync(path.join(root,ref.path))));
    if(value!==project.revision)throw Error('revision differs');pins++;
   }catch(error){issues.push('Source pin differs: '+project.id+' '+ref.path+'#'+ref.property);}
  }
 }
 for(const [id]of modules)if(!ids.has(id))issues.push('Undeclared Git module: '+id);
 let modulesChecked=0;
 if(!metadataOnly){
  if(!fs.existsSync(path.join(root,'.git')))issues.push('Initialize this source repository before checking Gitlinks; --metadata-only checks an uninitialized export.');
  else{
   const links=new Map();
   for(const line of git(['ls-files','--stage','--','src/external']).split('\n').filter(Boolean)){
    const match=line.match(/^160000 ([a-f0-9]{40}) 0\t(.+)$/);if(match)links.set(match[2],match[1]);else issues.push('External source is not a Gitlink: '+line.split('\t').at(-1));
   }
   for(const project of projects){
    const revision=links.get(project.path);
    if((!project.referenceOnly||revision)&&revision!==project.revision)issues.push('Gitlink differs or is missing: '+project.path);
    if(revision)modulesChecked++;
    const checkout=path.join(root,project.path);
    if(fs.existsSync(path.join(checkout,'.git'))){
     try{
      const head=git(['-C',checkout,'rev-parse','HEAD']);
      if(head!==project.revision)issues.push('Checkout revision differs: '+project.path);
      if(git(['-C',checkout,'status','--porcelain','--untracked-files=no']))issues.push('Checkout has tracked changes: '+project.path);
     }catch(error){issues.push('Cannot inspect supplier checkout: '+project.path);}
    }
   }
   for(const [name]of links)if(!declared.has(name))issues.push('Unregistered Gitlink: '+name);
  }
 }
 let sourceModules=0;
 for(const file of files(path.join(root,'src')).filter(file=>/\.(?:mjs|cjs|js)$/.test(file))){
  sourceModules++;
  for(const name of relativeImports(file)){
   const target=resolveImport(file,name);
   if(!target||!target.startsWith(root+path.sep))issues.push('Missing source import: '+path.relative(root,file)+' '+name);
  }
  for(const name of moduleImports(fs.readFileSync(file,'utf8')))if(!name.startsWith('.')&&!name.startsWith('node:'))issues.push('External package import: '+path.relative(root,file)+' '+name);
 }
 if(issues.length)throw Error(issues.join('\n'));
 return {sourceModules,projects:projects.length,pins,gitlinks:modulesChecked,metadataOnly};
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url)){
 const args=process.argv.slice(2);if(args.some(arg=>arg!=='--metadata-only'))throw Error('Usage: node tools/check-sources.mjs [--metadata-only]');
 console.log(JSON.stringify(checkSources(path.resolve(import.meta.dirname,'..'),{metadataOnly:args.includes('--metadata-only')})));
}
