import fs from 'node:fs';
import path from 'node:path';

// Allocation checks around the pinned implementation; demux and codec logic
// remain upstream. The external checkout stays read-only.
export function prepareMpegSource(klepton, build) {
  let source=fs.readFileSync(path.join(klepton,'runtime/media/kl_mediaplayer.c'),'utf8');
  const edits=[
    ['void kl_mediaplayer_prepare(kl_mediaplayer *mp) {',
      'static void klj_mpeg_prepare_data(kl_mediaplayer *mp, const unsigned char *f, long n);\n\nvoid kl_mediaplayer_prepare(kl_mediaplayer *mp) {'],
    ['if (n <= 0) { fclose(fp); return; }', 'if (n <= 0 || n > 64L * 1024 * 1024) { fclose(fp); return; }'],
    ['    // Two shapes reach MediaPlayer:',
      '    klj_mpeg_prepare_data(mp, f, n);\n    free(f);\n}\n\nstatic void klj_mpeg_prepare_data(kl_mediaplayer *mp, const unsigned char *f, long n) {\n    // Two shapes reach MediaPlayer:'],
    ['    free(f);\n    if (!mp->es', '    if (!mp->es'],
    ['unsigned char *es = malloc(n);', 'unsigned char *es = malloc(n);\n    if (!es) { *out = NULL; *out_len = 0; return; }'],
    ['mp->pkts = malloc(cap * sizeof *mp->pkts);', 'mp->pkts = malloc(cap * sizeof *mp->pkts);\n    if (!mp->pkts) return -1;'],
    ['if (mp->npkts == cap) { cap *= 2; mp->pkts = realloc(mp->pkts, cap * sizeof *mp->pkts); }',
      'if (mp->npkts == cap) {\n            long next = cap * 2;\n            void *grown = realloc(mp->pkts, next * sizeof *mp->pkts);\n            if (!grown) return -1;\n            mp->pkts = grown; cap = next;\n        }'],
    ['if (!mp->queue) return;\n    pthread_mutex_lock(&mp->mu);',
      'if (!mp->queue) return;\n    for (int i = 0; i < KL_MP_NBUF; i++) if (!mp->bufs[i]) return;\n    pthread_mutex_lock(&mp->mu);']
  ];
  for(const [before,after] of edits){
    if(source.split(before).length!==2)throw Error('Klepton MPEG allocation adapter needs an upstream update');
    source=source.replace(before,after);
  }
  const output=path.join(build,'kl_mediaplayer.c');fs.writeFileSync(output,source);return output;
}
