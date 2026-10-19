import fs from 'node:fs';
import path from 'node:path';
/** Preserve Klepton's compressed sample conversion in the sole AVFoundation
 * extractor. Its previous extractor object/table and demux ownership are retired. */
export function prepareExtractorBufferSource(klepton,output){
  const source=fs.readFileSync(path.join(klepton,'runtime/media/kl_avdec.m'),'utf8');
  const begin=source.indexOf('static const uint8_t k_startcode[4]'),end=source.indexOf('// Pull the next sample into',begin);
  if(begin<0||end<begin)throw Error('Pinned native sample conversion changed');
  const state=`typedef struct {
    unsigned char *psets;size_t psets_len;int nal_len_size;
    unsigned char *cur;size_t cur_len;long long cur_pts_us;int cur_key,eos;
} KljSampleBuffer;
`;
  fs.mkdirSync(output,{recursive:true});
  fs.writeFileSync(path.join(output,'KleptonDemuxBuffer.inc'),state+source.slice(begin,end).replaceAll('kl_avdemux','KljSampleBuffer'));
}
export function retireDemuxSource(source){
  const begin=source.indexOf('// Demux-only path for Unity');
  if(begin<0)throw Error('Pinned native demux ownership changed');
  return source.slice(0,begin);
}
export function mediaExtractorSource(source){
  const begin=source.indexOf('// AMediaExtractor — the DEMUXER'),end=source.indexOf('// AHardwareBuffer —',begin);
  if(begin<0||end<begin)throw Error('Pinned native extractor implementation changed');
  source=source.slice(0,begin)+source.slice(end);
  const reportStart=source.indexOf('    // Never silent once anything media-shaped has happened.');
  const reportEnd=source.indexOf('    fprintf(f, "  codec ',reportStart);
  if(reportStart<0||reportEnd<reportStart)throw Error('Pinned native extractor diagnostics changed');
  source=source.slice(0,reportStart)+'    if (!g_ever_created) return;\n    fprintf(f, "\\n=== media (AMediaCodec / AImageReader) ===\\n");\n'+source.slice(reportEnd);
  source='#include '+JSON.stringify(path.join(import.meta.dirname,'media-data-source.h'))+'\n'+source;
  source=source.replace(/^\s*M\("AMediaExtractor_[^\n]+\n/gm,'');
  const lookup='void *kl_mediandk_lookup(const char *name) {';
  if(source.split(lookup).length!==2)throw Error('Pinned native media lookup changed');
  return source.replace(lookup,lookup+`\n    if(name&&!strncmp(name,"AMediaExtractor_",16))return klj_media_format_lookup(name);
    if(name){void *source=klj_media_source_lookup(name);if(source)return source;}`);
}
