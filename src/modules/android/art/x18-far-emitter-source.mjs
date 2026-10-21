/** Add a far-address variant of the already generated pure instruction emitter.
 * All decoding, scratch ownership, instruction bodies and local conditional
 * offsets remain in the original function. Only its one-word B sinks differ. */
export function farEmitterSource(emit) {
  const name = 'static int klj_x18_emit_surveyed(';
  const start = emit.indexOf(name);
  const signature = 'unsigned slot, uint64_t ctr) {';
  if (start < 0 || emit.indexOf(name, start + name.length) !== -1)
    throw Error('The shared far register emitter boundary changed.');
  const opening = emit.indexOf('{', start);
  let end = -1, depth = 0, quoted = '', comment = '';
  for (let at = opening; at >= 0 && at < emit.length; at++) {
    const character = emit[at], next = emit[at + 1];
    if (comment === 'line') { if (character === '\n') comment = ''; continue; }
    if (comment === 'block') { if (character === '*' && next === '/') { comment = ''; at++; } continue; }
    if (quoted) { if (character === '\\') at++; else if (character === quoted) quoted = ''; continue; }
    if (character === '/' && next === '/') { comment = 'line'; at++; continue; }
    if (character === '/' && next === '*') { comment = 'block'; at++; continue; }
    if (character === '"' || character === "'") { quoted = character; continue; }
    if (character === '{') depth++;
    if (character === '}' && --depth === 0) { end = at + 1; break; }
  }
  let copy = emit.slice(start, end).trim();
  if (end < 0 || copy.split(signature).length !== 2 ||
      !/\benc_b\s*\(/.test(copy))
    throw Error('The shared far register emitter signature changed.');
  copy = copy.replace(name, 'static int klj_x18_emit_surveyed_far(')
    .replace(signature, 'unsigned slot, uint64_t ctr, KljX18FarSink *sink) {')
    .replace(/\benc_b\s*\(/g, 'klj_x18_far_branch(sink,');
  return `
typedef struct {
    uint64_t site;
    uint16_t entry_imm;
    uint64_t *branches;
    size_t capacity, count;
    int failed;
} KljX18FarSink;

/* Capacity counts {pc,target} pairs, not uint64_t elements. The caller retains
 * both the body and these immutable targets before exposing the entry word. */
static int klj_x18_far_branch(KljX18FarSink *sink, uint64_t from,
                              uint64_t target, uint32_t *word) {
    if (from == sink->site) {
        *word = 0xd4200000u | ((uint32_t)sink->entry_imm << 5);
        return 1;
    }
    *word = 0xd4200000u | (0xa0dfu << 5);
    if (!sink->branches || sink->count >= sink->capacity) {
        sink->failed = 1;
        return 0;
    }
    sink->branches[sink->count * 2] = from;
    sink->branches[sink->count * 2 + 1] = target;
    sink->count++;
    return 1;
}

${copy}

int klx_core_emit_far(uint32_t word, uint64_t site, uint64_t pool,
                      unsigned slot, uint64_t ctr, uint16_t entry_imm,
                      uint32_t *words, size_t capacity, uint64_t *branches,
                      size_t branch_capacity, size_t *branch_count) {
    if (!branch_count) return -1;
    *branch_count = 0;
    if (!words || !capacity || slot >= 4096 || (site & 3) || (pool & 3) ||
        entry_imm == 0xa0dfu || capacity > (size_t)-1 / 4 ||
        branch_capacity > (size_t)-1 / 2 || site > UINT64_MAX - 4 ||
        pool > UINT64_MAX - capacity * 4 ||
        (site >= pool && site < pool + capacity * 4)) return -1;
    klx_info info;
    klx_decode(word, &info);
    if (!info.nfields) return -1;
    kl_x18_stats stats;
    size_t used = 0;
    KljX18FarSink sink = {site, entry_imm, branches, branch_capacity, 0, 0};
    int result = klj_x18_emit_surveyed_far(&word, 4, site, words, capacity * 4,
        pool, &stats, &used, 1, 0, info.sysreg != 0, NULL, slot, ctr, &sink);
    if (result || sink.failed || stats.refused || stats.ctr_refused ||
        !(stats.patched + stats.ctr_patched)) return -1;
    *branch_count = sink.count;
    return (int)(used / 4);
}
`;
}
