import fs from 'node:fs';import path from 'node:path';import {createHash}from'node:crypto';
import {expandPolicy} from './policy-expand.mjs';
export function prepareAuthorization({output:out,source}) {
expandPolicy({output:out,source});
const text=fs.readFileSync(out+'/policy-expanded.conf','utf8').replace(/#[^\n]*/g,'');
// Actual authenticated principals, supplied by the platform supervisor. These
// are security subjects; the table never controls Android API availability.
const roles=['untrusted_app','system_server','keystore'];
const types=new Map();
for(const match of text.matchAll(/\btype\s+(\w+)\s*((?:,\s*\w+\s*)*);/g))types.set(match[1],new Set([match[1],...match[2].split(',').map(x=>x.trim()).filter(Boolean)]));
for(const match of text.matchAll(/\btypeattribute\s+(\w+)\s+(\w+)\s*;/g)){
 const attrs=types.get(match[1]);if(attrs)attrs.add(match[2]);
}
for(const role of roles)if(!types.has(role))throw Error('Original policy lacks authenticated role: '+role);
const namespaceContexts=fs.readFileSync(source+'/private/keystore2_key_contexts','utf8').split('\n').map(line=>line.replace(/#.*/,'').trim()).filter(Boolean).map(line=>{const[id,label]=line.split(/\s+/);return{id:Number(id),label};});
const serviceContexts=fs.readFileSync(out+'/service-contexts-expanded','utf8').split('\n').map(line=>line.replace(/#.*/,'').trim()).filter(Boolean).map(line=>{const[name,label]=line.split(/\s+/);if(!name||!label||!label.startsWith('u:object_r:'))throw Error('Invalid original service context '+line);return{name,label};});
const targetLabels=new Set(['u:r:keystore:s0','u:r:servicemanager:s0',...namespaceContexts.map(x=>x.label),...serviceContexts.map(x=>x.label)]);
const classes=new Map();for(const m of text.matchAll(/\bclass\s+(keystore2_key|keystore2|service_manager)\s*\{([^}]+)\}/g))classes.set(m[1],m[2].trim().split(/\s+/));
if(classes.size!==3)throw Error('Original userspace permission classes missing');
function match(expression,attributes,all){
 expression=expression.trim();if(expression[0]==='~')return !match(expression.slice(1),attributes,all);
 const words=expression.replace(/[{}]/g,' ').trim().split(/\s+/),positive=words.filter(x=>!x.startsWith('-')),negative=words.filter(x=>x.startsWith('-')).map(x=>x.slice(1));
 return positive.some(x=>x==='*'||attributes.has(x))&&!negative.some(x=>attributes.has(x));
}
const term='(?:~?\\s*\\{[^}]*\\}|[\\w*]+)';
const allows=new RegExp('\\ballow\\s+('+term+')\\s+('+term+')\\s*:\\s*('+term+')\\s+('+term+')\\s*;','g');
const rules=[];
for(const m of text.matchAll(allows))for(const role of roles){
 if(!match(m[1],types.get(role)))continue;
 for(const label of targetLabels){
  const type=label.split(':')[2],attrs=new Set(types.get(type)||[type]);if(type===role)attrs.add('self');
  if(!match(m[2],attrs))continue;
  for(const [kind,permissions]of classes)if(match(m[3],new Set([kind])))for(const permission of permissions)if(match(m[4],new Set([permission])))rules.push({source:'u:r:'+role+':s0',target:label,class:kind,permission});
 }
}
const unique=[...new Map(rules.map(r=>[[r.source,r.target,r.class,r.permission].join('\0'),r])).values()];
let header='// Original AOSP m4-expanded policy projected onto actual authenticated principals.\n';
header+='static const struct Rule {const char *source,*target,*tclass,*permission;} rules[]={\n'+unique.map(r=>'{'+[r.source,r.target,r.class,r.permission].map(JSON.stringify).join(',')+'},').join('\n')+'\n};\n';
header+='static const struct Context {long namespace_id;const char *label;} contexts[]={\n'+namespaceContexts.map(c=>'{'+c.id+','+JSON.stringify(c.label)+'},').join('\n')+'\n};\n';
header+='static const struct ServiceContext {const char *name,*label;} service_contexts[]={\n'+serviceContexts.map(c=>'{'+JSON.stringify(c.name)+','+JSON.stringify(c.label)+'},').join('\n')+'\n};\n';
fs.writeFileSync(out+'/policy.h',header);
const inputs=JSON.parse(fs.readFileSync(out+'/policy-inputs.json'));
fs.writeFileSync(out+'/policy.json',JSON.stringify({source,roleAttributes:Object.fromEntries(roles.map(r=>[r,[...types.get(r)]])),rules:unique,contexts:namespaceContexts,serviceContexts,definitions:inputs.definitions,inputs:inputs.files.map(file=>({file:path.relative(source,file),sha256:createHash('sha256').update(fs.readFileSync(file)).digest('hex')})),boundary:'Original userspace KeyStore and service-manager authorization; no kernel SELinux state or process relabeling.'},null,2)+'\n');
return {include:out,rules:unique,contexts:namespaceContexts,serviceContexts};
}
