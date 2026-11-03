import {addLicense} from '../upstream/licenses.mjs';
import {writeChanged} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {exportSources} from '../binder-rpc/build.mjs';
import {compileHostObject} from '../host-compile.mjs';

const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));

/** Locale objects and strings retain their existing physical owner. Only the
 * returned record uses Android's original field ordering and declaration. */
export function prepareNativeLocale({bionicSource,build,ninja,python}){
  const output=path.join(build,'native-locale'),source=path.join(output,'source');
  exportSources(bionicSource,pins.aospBionic,['libc/include/locale.h','libc/NOTICE'],source);
  const header=fs.readFileSync(path.join(source,'libc/include/locale.h'),'utf8');
  const record=header.match(/struct lconv \{[^}]+\};/)?.[0];
  if(!record)throw Error('Original Bionic locale record is absent');
  const fields=[...record.matchAll(/\b(\w+)\s*;/g)].map(match=>match[1]);
  if(!fields.length)throw Error('Original Bionic locale fields are absent');
  writeChanged(path.join(output,'android-locale.h'),record.replace('struct lconv','struct klj_android_lconv').replace(/\b_Nonnull\b/g,'')+'\n#define KLJ_LOCALE_FIELDS(X) '+fields.map(name=>'X('+name+')').join(' ')+'\n');
  const unit=path.join(import.meta.dirname,'locale.c');
  const object=compileHostObject({name:'bionic-locale-record.o',inputs:[unit],flags:['-std=c11','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-Wall','-Wextra','-Werror','-I'+output],directory:path.join(build,'host-objects'),cwd:source,ninja,python}).output;
  const licenses=path.join(output,'licenses');
  addLicense(path.dirname(licenses),'native-locale',path.join(source,'libc/NOTICE'));
  return {output,objects:[object],names:['localeconv'],licenses};
}
