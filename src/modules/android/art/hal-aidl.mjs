import path from 'node:path';
import {execFileSync} from 'node:child_process';

/** Resolve the original HAL's full frozen-AIDL dependency closure using the
 * upstream generator and manifests, rather than maintaining another API list. */
export function generateArtHalAidl({aim,aidl,python,hal,apiRoot=path.join(aim,'_build/aosp/aidl_api')}){
 if(!aidl||!python)throw Error('The HAL build requires its AIDL compiler and Python');
 execFileSync(python,['-c',`import importlib.util,pathlib,shlex,sys,tomllib
root=pathlib.Path(sys.argv[1]);spec=importlib.util.spec_from_file_location('original_hal',root/'tools/lib/vendor_hal_aidl.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
entries=shlex.split((root/'hal/sources.lock').read_text().split('AIDL_INTERFACES=(',1)[1].split('\\n)',1)[0],comments=True)
interfaces=m.Interfaces(str(pathlib.Path(sys.argv[4])),entries)
dependencies=tomllib.loads((root/'hal'/sys.argv[3]/'Cargo.toml').read_text())['dependencies']
wanted=set()
for name in interfaces.versions:
 if m.crate(name) in dependencies:wanted.update([name,*interfaces.transitive(name)])
for name in sorted(wanted):m.generate(sys.argv[2],interfaces,name,str(root/'target/aim/gen/hal-aidl'),str(root/'hal/aidl'))
`,path.resolve(aim),path.resolve(aidl),hal,path.resolve(apiRoot)],{stdio:'inherit'});
}

