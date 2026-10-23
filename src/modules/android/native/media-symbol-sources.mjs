import fs from 'node:fs';
import path from 'node:path';
import {openSlIdSource} from './opensl-ids.mjs';
import {openSlConfigurationSource} from './opensl-configuration.mjs';
import {audioActivity} from './native-surface-sources.mjs';
import {adaptOpenSlCapture} from './audio-capture-sources.mjs';
import {mediaCodecSource} from './media-codec-source.mjs';
import {videoDecoderSource,videoDecoderHeader} from './video-decoder-source.mjs';
import {mediaFormatSource} from './media-format-source.mjs';
import {mediaExtractorSource,retireDemuxSource} from './media-extractor-source.mjs';
import {retireTraceSamples} from './binder-rpc/tracing-sources.mjs';

function replace(source,before,after){
  if(source.split(before).length!==2)throw Error('Pinned native media source needs an upstream update: '+before.slice(0,80));
  return source.replace(before,after);
}

/** dlsym reports real exports; unresolved ELF relocations retain named diagnostics. */
export function prepareMediaSymbolSources(klepton,build){
  const edits={
    'runtime/media/kl_opensl.c':['    sl_slot(name);\n    return kl_named_stub(name, (void *)klsl_called);','    return NULL;'],
    'runtime/media/kl_mediandk.c':['    return fn ? fn : kl_named_stub(name, (void *)kl_unresolved_named);','    return fn;']
  };
  const files={};
  for(const[file,[before,after]]of Object.entries(edits)){
    const source=fs.readFileSync(path.join(klepton,file),'utf8');
    if(source.split(before).length!==2)throw Error('Pinned native symbol lookup needs an upstream update: '+file);
    const output=path.join(build,path.basename(file));
    let adapted=source.replace(before,after);
    if(file==='runtime/media/kl_opensl.c')adapted=openSlConfigurationSource(openSlIdSource(audioActivity(adaptOpenSlCapture(adapted),'p','SL_PLAYSTATE_PLAYING')));
    if(file==='runtime/media/kl_mediandk.c')adapted=retireTraceSamples(mediaFormatSource(mediaCodecSource(mediaExtractorSource(adapted))));
    fs.writeFileSync(output,adapted);files[file]=output;
  }
  const demux='runtime/media/kl_avdec.m',output=path.join(build,'kl_avdec.m');
  fs.writeFileSync(output,retireDemuxSource(fs.readFileSync(path.join(klepton,demux),'utf8')));files[demux]=output;
  fs.writeFileSync(path.join(build,'kl_vtdec.h'),videoDecoderHeader(fs.readFileSync(path.join(klepton,'runtime/media/kl_vtdec.h'),'utf8')));
  const decoder='runtime/media/kl_vtdec.c',decoderOutput=path.join(build,'kl_vtdec.c');
  fs.writeFileSync(decoderOutput,videoDecoderSource(fs.readFileSync(path.join(klepton,decoder),'utf8')));files[decoder]=decoderOutput;
  return files;
}
