import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from '../native/upstream/source-files.mjs';

/** Linux BTI support includes guarded executable mappings, not just CPU hints. */
export function prepareHardwareCapabilities(aim){
 const file=path.join(aim,'crates/aim-linux-abi/src/hwcap.rs');
 const source=fs.readFileSync(file,'utf8'),from='    set(arm("FEAT_BTI"), 17, true);';
 if(source.split(from).length!==2)throw Error('AIM BTI capability boundary changed');
 writeChanged(file,source.replace(from,`    // Darwin CPU BTI support does not provide Linux PROT_BTI guarded pages.
    // The memory adapter currently discards that flag. Keep the original
    // loader's unguarded path until guarded execution is implemented; BTI
    // instruction hints and all other CPU capabilities remain unchanged.`));
}
