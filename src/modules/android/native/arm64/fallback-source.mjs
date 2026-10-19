import fs from 'node:fs';
import path from 'node:path';

export function prepareA64Fallback({source,output}){
 const file=path.join(source,'src/dynarmic/backend/arm64/a64_address_space.cpp');
 let text=fs.readFileSync(file,'utf8');
 const before='    Optimization::VerificationPass(ir_block);\n\n    return ir_block;';
 if(text.split(before).length!==2)throw Error('Dynarmic A64 fallback boundary changed');
 text=text.replace(before,`    Optimization::VerificationPass(ir_block);
    const IR::Terminal terminal=ir_block.GetTerminal();
    if(const auto* interpret=boost::get<IR::Term::Interpret>(&terminal)) {
        // This backend has no Interpret terminal emitter. Let the existing
        // callback stop execution or provide its interpreter before dispatch.
        conf.callbacks->InterpreterFallback(A64::LocationDescriptor{interpret->next}.PC(),interpret->num_instructions);
        IR::Block stopped{descriptor};
        stopped.SetTerminal(IR::Term::ReturnToDispatch{});
        stopped.CycleCount()=1;
        return stopped;
    }
    return ir_block;`);
 fs.mkdirSync(path.dirname(output),{recursive:true});
 if(!fs.existsSync(output)||fs.readFileSync(output,'utf8')!==text)fs.writeFileSync(output,text);
 return output;
}
if(process.argv[1]&&path.resolve(process.argv[1])===import.meta.filename)
 prepareA64Fallback({source:path.resolve(process.argv[2]),output:path.resolve(process.argv[3])});
