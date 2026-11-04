import {prepareSourceTree} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {exportSources} from '../binder-rpc/build.mjs';
import {prepareKeyStoreSources} from './source-export.mjs';
import {buildKeyMintFoundation} from './keymint-foundation.mjs';
import {generateKeyMintAidl} from './aidl.mjs';
import {buildKeyMintAidl} from './keymint-aidl.mjs';
import {prepareNativeHal} from './hal-sources.mjs';
import {buildNativeHal} from './hal-build.mjs';
import {buildKeyStoreRust} from './rust/build.mjs';
import {buildAttestationIds} from './attestation-id.mjs';
import {linkKeyStore} from './link.mjs';

const nativeRoot=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const pins=JSON.parse(fs.readFileSync(path.join(nativeRoot,'source-pins.json')));
const catalog=JSON.parse(fs.readFileSync(new URL('./sources.json',import.meta.url)));

/**
 * Prepare the shared native owners before Binder is linked. KeyStore and
 * Conscrypt use the same BoringSSL, and Binder owns libbase/liblog once.
 */
export async function prepareKeyStoreFoundation({output,checkouts={},boringssl,aidl,aconfig,rustToolchain,ninja,python,processLibrary}) {
  output=path.resolve(output);
  checkouts={...Object.fromEntries(catalog.map(record=>[
    record.name,path.resolve(nativeRoot,'../../../external',record.checkout),
  ])),...checkouts};
  const prepared=prepareKeyStoreSources({checkouts,output});
  const sources={...prepared.sources,framework:prepared.sources['java-framework']};
  for(const [name,revision,trees]of[
    ['bionic',pins.aospBionic,['libc/include/sys/system_properties.h']],
    ['boringssl',pins.aospBoringSSL,['src/rust']],
  ]){
    sources[name]=path.join(output,'source',name);
    prepareSourceTree(sources[name],temporary=>exportSources(checkouts[name],revision,trees,temporary));
  }
  const foundation=await buildKeyMintFoundation({sources:checkouts,boringssl,
    bionic:sources.bionic,output:path.join(output,'foundation'),ninja,python});
  const keyMintAidl=generateKeyMintAidl({source:sources['hardware-interfaces'],
    compiler:aidl,output:path.join(output,'keymint-aidl-generated')});
  const context={
    output,
    checkouts,
    sources,
    foundation,
    keyMintAidl,
    boringssl,
    aidl,
    aconfig,
    rustToolchain,
    ninja,
    python,
    processLibrary
  };
  return {foundation,context};
}

/** Link the complete original services after the shared Binder owner exists. */
export async function buildKeyStore(context) {
  const {
    output,
    checkouts,
    foundation,
    keyMintAidl,
    binder,
    boringssl,
    aidl,
    processLibrary
  }=context;
  const sources={...context.sources,...foundation.sources,
    // Binder exports the complete original native directory and OS headers.
    native:path.join(binder.output,'source/native'),
    core:path.join(binder.output,'source/core'),
  };
  const wrapper=await buildKeyMintAidl({output:path.join(output,'keymint-aidl'),
    backend:foundation,generated:keyMintAidl,ninja:context.ninja,python:context.python,
    binderSource:path.join(binder.output,'source'),
    binderNdkAdaptation:path.join(binder.output,'ndk/include')});
  const nativeHal=buildNativeHal({wrapper,prepared:prepareNativeHal({
    output:path.join(output,'hal'),security:sources.security,
    hardware:sources['hardware-interfaces'],compiler:aidl,keyMintAidl,
  })});
  const rust=buildKeyStoreRust({...context,sources,nativeHal});
  const attestation=buildAttestationIds({framework:sources.framework,
    security:sources.security,compiler:aidl,output:path.join(output,'attestation'),
    keyStoreAidl:rust.aidl.records.find(record=>record.name==='android.system.keystore2'),
    keyMintAidl,wrapper,binder});
  const linked=linkKeyStore({output:path.join(output,'native'),sources,
    nativeCheckout:checkouts.native,nativeRevision:pins.aospNative,binder,
    foundation,wrapper,rustLibrary:rust.rustLibrary,attestation,nativeHal,
    crypto:{...rust.crypto,crypto:boringssl.crypto},processLibrary});
  return {
    ...linked
  };
}
