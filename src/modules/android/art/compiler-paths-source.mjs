import fs from 'node:fs';
import path from 'node:path';
import {nativePathMappings} from '../native/upstream/native-paths.mjs';
import {writeChanged} from '../native/upstream/source-files.mjs';

/** Keep original ART compiler commands while mapping their source identities. */
export function adaptArtCompilerPaths(source,{roots=[]}={}){
 const replace=(before,after)=>{
  if(source.split(before).length!==2)throw Error('ART compiler path source changed');
  source=source.replace(before,()=>after);
 };
 const declared=JSON.stringify(Object.fromEntries(nativePathMappings({roots})));
 replace('    def rules(self):\n        clang =',`    def rules(self):
        mappings = ${declared}

        def map_path(directory, replacement):
            for original in (os.path.abspath(directory), os.path.realpath(directory)):
                mappings[original] = replacement

        map_path(os.path.expanduser("~"), "/home/build")
        map_path(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "/src/aim")
        map_path(self.out, "/build/art")
        map_path(self.ndk, "/toolchain/android-ndk")
        map_path(self.toolchain, "/toolchain/android-ndk/toolchains/llvm/prebuilt/darwin-x86_64")
        map_path(self.src_root, "/src/aosp")
        # Fetched trees are linked beside the staged ART tree. Their physical
        # header paths need the same identity as their source-root aliases.
        for name in sorted(os.listdir(self.src_root)):
            map_path(os.path.join(self.src_root, name), "/src/aosp/" + name)
        portable = []
        for original, replacement in sorted(mappings.items(), key=lambda item: (len(item[0]), item[0])):
            portable.extend(("-ffile-prefix-map=" + original + "=" + replacement,
                             "-fdebug-prefix-map=" + original + "=" + replacement))
        portable.append("-ffile-compilation-dir=/build")
        self.emit("path_flags = " + shlex.join(portable).replace("$", "$$"))
        clang =`);
 for(const compiler of ['$cxx','$cc']){
  const count=compiler==='$cc'?2:1;
  const before='command = '+compiler+' $target -MMD';
  if(source.split(before).length!==count+1)throw Error('ART compiler command source changed');
  source=source.replaceAll(before,'command = '+compiler+' $target $path_flags -MMD');
 }
 return source;
}

export function prepareArtCompilerPaths(aim,{source}={}){
 const file=path.join(aim,'tools/art-android/gen_build.py');
 writeChanged(file,adaptArtCompilerPaths(fs.readFileSync(file,'utf8'),{roots:source?[[source,'/src/aim']]:[]}));
}
