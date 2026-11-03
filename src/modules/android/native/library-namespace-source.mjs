import {copyOptional,writeOptional,prepareSourceTree,writeChanged} from './upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {exportSources} from './binder-rpc/build.mjs';

/** Keep original Bionic namespace access semantics and original ART flags.
 * Only the image registry's surrounding namespace representation differs. */
export function prepareLibraryNamespaces({bionicSource,artSource,output}){
 const pins=JSON.parse(fs.readFileSync(new URL('./source-pins.json',import.meta.url)));
 prepareSourceTree(output,temporary=>{
  const bionic=path.join(temporary,'bionic'),art=path.join(temporary,'art');
  exportSources(bionicSource,pins.aospBionic,['linker/linker_namespaces.cpp','linker/linker_utils.cpp','libc/NOTICE'],bionic);
  exportSources(artSource,pins.aospArt,['libnativeloader/include/nativeloader/dlext_namespaces.h','libnativeloader/library_namespaces.cpp','libnativebridge/include/nativebridge/native_bridge.h'],art);
  const namespace=fs.readFileSync(path.join(bionic,'linker/linker_namespaces.cpp'),'utf8');
  const utils=fs.readFileSync(path.join(bionic,'linker/linker_utils.cpp'),'utf8');
  const api=fs.readFileSync(path.join(art,'libnativeloader/include/nativeloader/dlext_namespaces.h'),'utf8');
  const loader=fs.readFileSync(path.join(art,'libnativeloader/library_namespaces.cpp'),'utf8');
  function slice(text,start,end){const first=text.indexOf(start),last=text.indexOf(end,first+start.length);if(first<0||last<0)throw Error('Original native namespace source changed: '+start);return text.slice(first,last);}
  const checks=slice(utils,'bool file_is_in_dir(', 'const char* const kZipFileSeparator').replaceAll('bool file_is_','static bool file_is_');
  const runpath=slice(utils,'void format_string(', 'bool normalize_path(').replace('void format_string(', 'static void format_string(').replace('std::string dirname(', 'static std::string libraryDirectory(');
  const access=slice(namespace,'bool android_namespace_t::is_accessible(const std::string& file)', '// Are symbols from this shared object')
   .replace('android_namespace_t::','KljLibraryNamespace::').replace('basename(file.c_str())','baseName(file.c_str())');
  const flags=slice(api,'enum {','/*\n * Creates new linker namespace.');
  writeChanged(path.join(temporary,'bionic-namespace.inc'),namespace.slice(0,namespace.indexOf('#include'))+checks+'\n'+runpath+'\n'+access+'\n'+flags);
  const apex=slice(loader,'constexpr const char* kApexPath =','\n');
  const location=slice(loader,'std::optional<std::string> FindApexNamespaceName(', '\n}  // namespace android::nativeloader');
  writeChanged(path.join(temporary,'art-namespace.inc'),loader.slice(0,loader.indexOf('*/')+2)+'\n'+apex+'\n'+location);
  const notices=path.join(temporary,'notices');fs.mkdirSync(notices);
  copyOptional(path.join(bionic,'libc/NOTICE'),path.join(notices,'bionic.txt'));
  writeOptional(path.join(notices,'art-native-loader.txt'),api.slice(0,api.indexOf('#ifndef')));
 });
 return {include:output,nativeLoaderInclude:path.join(output,'art/libnativeloader/include'),nativeBridgeInclude:path.join(output,'art/libnativebridge/include'),notices:path.join(output,'notices')};
}
