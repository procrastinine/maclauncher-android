import fs from 'node:fs';
import path from 'node:path';

function replace(text,before,after){
  if(text.split(before).length!==2)throw Error('Unexpected Dynarmic FP state source: '+before.slice(0,80));
  return text.replace(before,after);
}
function write(file,text){
  fs.mkdirSync(path.dirname(file),{recursive:true});
  if(!fs.existsSync(file)||fs.readFileSync(file,'utf8')!==text)fs.writeFileSync(file,text);
  return file;
}

// Keep FP modes with the original FPSR owner. Consecutive NEON operations may
// retain their standard mode until an original spill/scalar/control boundary.
// No guest mode, arithmetic operation, status accumulation or option changes.
export function prepareFpcrSources({source,output}){
  const backend=path.join(source,'src/dynarmic/backend/arm64');
  let header=fs.readFileSync(path.join(backend,'fpsr_manager.h'),'utf8');
  header=replace(header,'    void Load();','    void Load();\n    void LoadPreservingFpcr();\n    void SetFpcr(u32 value, u32 original);');
  header=replace(header,'    void Overwrite() { fpsr_loaded = false; }','    void Overwrite() { RestoreFpcr(); fpsr_loaded = false; }');
  header=replace(header,'    bool fpsr_loaded = false;','    bool fpsr_loaded = false;\n    u32 original_fpcr = 0;\n    u32 active_fpcr = 0;\n    bool fpcr_changed = false;\n    void RestoreFpcr();');
  let manager=fs.readFileSync(path.join(backend,'fpsr_manager.cpp'),'utf8');
  manager=replace(manager,'void FpsrManager::Spill() {','void FpsrManager::Spill() {\n    RestoreFpcr();');
  manager=replace(manager,'void FpsrManager::Load() {',`void FpsrManager::RestoreFpcr() {
    if (!fpcr_changed) return;
    code.MOV(Wscratch0, original_fpcr);
    code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
    fpcr_changed = false;
}

void FpsrManager::SetFpcr(u32 value, u32 original) {
    if (fpcr_changed ? value == active_fpcr : value == original) return;
    code.MOV(Wscratch0, value);
    code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
    original_fpcr = original;
    active_fpcr = value;
    fpcr_changed = value != original;
}

void FpsrManager::Load() {
    RestoreFpcr();
    LoadPreservingFpcr();
}

void FpsrManager::LoadPreservingFpcr() {`);
  let vector=fs.readFileSync(path.join(backend,'emit_arm64_vector_floating_point.cpp'),'utf8');
  vector=replace(vector,`    if (ctx.FPCR(fpcr_controlled) != ctx.FPCR()) {
        code.MOV(Wscratch0, ctx.FPCR(fpcr_controlled).Value());
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
        emit();
        code.MOV(Wscratch0, ctx.FPCR().Value());
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
    } else {
        emit();
    }`,`    (void)code;
    ctx.fpsr.SetFpcr(ctx.FPCR(fpcr_controlled).Value(), ctx.FPCR().Value());
    emit();
    // The optional original instruction logger calls native code without a
    // normal allocation spill. Retain its original per-instruction mode.
    if (ctx.conf.very_verbose_debugging_output)
        ctx.fpsr.SetFpcr(ctx.FPCR().Value(), ctx.FPCR().Value());`);
  const loads=(vector.match(/ctx\.fpsr\.Load\(\);/g)||[]).length;
  if(loads!==8)throw Error('Unexpected original vector FPSR load count: '+loads);
  vector=vector.replaceAll('ctx.fpsr.Load();','ctx.fpsr.LoadPreservingFpcr();');
  return {
    include:output,
    header:write(path.join(output,'dynarmic/backend/arm64/fpsr_manager.h'),header),
    manager:write(path.join(output,'fpsr_manager.cpp'),manager),
    vector:write(path.join(output,'emit_arm64_vector_floating_point.cpp'),vector),
  };
}
if(process.argv[1]&&path.resolve(process.argv[1])===import.meta.filename)
  prepareFpcrSources({source:path.resolve(process.argv[2]),output:path.resolve(process.argv[3])});
