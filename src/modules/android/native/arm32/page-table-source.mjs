import fs from 'node:fs';
import path from 'node:path';

function replace(text,before,after){
  if(text.split(before).length!==2)throw Error('Unexpected Dynarmic page-table source: '+before.slice(0,80));
  return text.replace(before,after);
}

// Retain upstream memory instructions, register allocation, ordering, deferred
// callbacks and abort handling. Only resolve an additional bounded entry form
// in the original page-table lookup. Other configurations keep upstream code.
export function preparePageTableSource({source,output}){
  let text=fs.readFileSync(path.join(source,'src/dynarmic/backend/arm64/emit_arm64_memory.cpp'),'utf8');
  // Callback-only accesses need the same precise abort boundary as deferred
  // fast-memory callbacks. Stop before committing a failed load's result.
  const first='template<size_t bitsize>\nvoid CallbackOnlyEmitReadMemory';
  text=replace(text,first,`void CheckCallbackMemoryAbort(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    oaknut::Label end;
    ctx.conf.emit_check_memory_abort(code, ctx, inst, end);
    code.l(end);
}

${first}`);
  for(const name of ['Read','ExclusiveRead','Write','ExclusiveWrite']){
    const begin=text.indexOf('void CallbackOnlyEmit'+name+'Memory('),end=text.indexOf('\n}',begin);
    if(begin<0||end<0)throw Error('Dynarmic callback abort boundary changed: '+name);
    text=text.slice(0,end)+'\n    CheckCallbackMemoryAbort(code, ctx, inst);'+text.slice(end);
  }
  text=replace(text,'#include <utility>','#include <utility>\n#include "page-table.h"');
  text=replace(text,'oaknut::XReg Xaddr, const SharedLabel& fallback) {\n    const size_t valid_page_index_bits',
    'oaknut::XReg Xaddr, const SharedLabel& fallback, bool writing = false) {\n    const size_t valid_page_index_bits');
  text=replace(text,'    code.LDR(Xscratch0, Xpagetable, Xscratch0, LSL, 3);\n\n    if (ctx.conf.page_table_pointer_mask_bits != 0) {',`    code.LDR(Xscratch0, Xpagetable, Xscratch0, LSL, 3);

    // MacLauncher's shared ARM32 owner uses the existing attribute bit to
    // identify its compact edge-page cells. The mapping guard retains every
    // referenced original Region and permission vector through this access.
    if (ctx.conf.page_table_address_space_bits == 32 &&
        ctx.conf.page_table_pointer_mask_bits == 1 && !ctx.conf.absolute_offset_page_table) {
        using maclauncher::arm32::PageTableMapping;
        oaknut::Label direct, resolved;
        code.TBZ(Xscratch0, 0, direct);
        code.AND(Xscratch0, Xscratch0, ~u64(maclauncher::arm32::boundedPageTag));
        code.UBFX(Xscratch1, Xaddr, maclauncher::arm32::compactCellBits,
                  page_bits - maclauncher::arm32::compactCellBits);
        code.LDR(Xscratch0, Xscratch0, Xscratch1, LSL, 3);
        code.CBZ(Xscratch0, *fallback);
        code.LDR(Wscratch1, Xscratch0, offsetof(PageTableMapping, base));
        code.SUB(Wscratch1, Xaddr.toW(), Wscratch1);
        code.LDR(Wscratch2, Xscratch0, offsetof(PageTableMapping, accessible));
        code.ADD(Xscratch1, Xscratch1, bitsize / 8);
        code.CMP(Xscratch1, Xscratch2);
        code.B(HI, *fallback);
        code.SUB(Xscratch1, Xscratch1, bitsize / 8);
        // A relative Region page can differ from the guest page for compact
        // reservations. Its crossing keeps the original multi-page callback.
        code.AND(Xscratch2, Xscratch1, page_mask);
        code.CMP(Xscratch2, page_size - bitsize / 8);
        code.B(HI, *fallback);
        // Keep the checked byte offset live while indexing its permission
        // page. Reconstructing it after this check adds a load and subtraction
        // to every compact native access.
        code.LDR(Xscratch2, Xscratch0, offsetof(PageTableMapping, permissionBytes));
        code.ADD(Xscratch2, Xscratch2, Xscratch1, LSR, page_bits);
        code.LDRB(Wscratch2, Xscratch2);
        // Deferred callbacks can be more than 32 KiB away in a large block.
        // Match upstream's full conditional branch range; NZCV was spilled
        // before this lookup and is already scratch here.
        code.TST(Wscratch2, writing ? 2 : 4);
        code.B(EQ, *fallback);
        code.LDR(Xscratch0, Xscratch0, offsetof(PageTableMapping, bytes));
        code.B(resolved);
        code.l(direct);
        code.CBZ(Xscratch0, *fallback);
        code.AND(Xscratch1, Xaddr, page_mask);
        code.l(resolved);
        return std::make_pair(Xscratch0, Xscratch1);
    }

    if (ctx.conf.page_table_pointer_mask_bits != 0) {`);
  text=replace(text,'InlinePageTableEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);\n    EmitMemoryStr',
    'InlinePageTableEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback, true);\n    EmitMemoryStr');
  fs.mkdirSync(path.dirname(output),{recursive:true});
  if(!fs.existsSync(output)||fs.readFileSync(output,'utf8')!==text)fs.writeFileSync(output,text);
  return output;
}
if(process.argv[1]&&path.resolve(process.argv[1])===import.meta.filename)
  preparePageTableSource({source:path.resolve(process.argv[2]),output:path.resolve(process.argv[3])});
