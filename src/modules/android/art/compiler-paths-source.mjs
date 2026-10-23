import fs from 'node:fs';
import path from 'node:path';
import {nativePathMappings} from '../native/upstream/native-paths.mjs';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Keep original ART compiler commands while mapping their source identities. */
export function adaptArtCompilerPaths(source,{roots=[]}={}){
 const replace=(before,after)=>{
  if(source.split(before).length!==2)throw Error('ART compiler path source changed');
  source=source.replace(before,()=>after);
 };
 // The matching JVMTI plugin uses ART's original Android.bp module.
 replace('    "art/dex2oat/Android.bp",\n', '    "art/dex2oat/Android.bp",\n    "art/openjdkjvmti/Android.bp",\n');
 replace('        # dex2oat64: regenerates the boot image with the same reference encoding.',`        # libopenjdkjvmti links the rebuilt libart's internal interfaces.
        jvmti_objs = self.objects(
            "libopenjdkjvmti", self.srcs("libopenjdkjvmti"),
            flags_for("libopenjdkjvmti", [f"-I{os.path.join(self.art, 'openjdkjvmti/include')}"]))
        self.shared_lib("libopenjdkjvmti", jvmti_objs, [],
                        [libart, artbase, dexfile, libbase, libcxx_so] + libc)

        # dex2oat64: regenerates the boot image with the same reference encoding.`);
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
 const adapters=path.join(import.meta.dirname,'adapters');
 copyChanged(path.join(adapters,'art-android-0006-arm64-codegen-heap-base.patch'),path.join(aim,'patches/art-android/0006-arm64-codegen-heap-base.patch'));
 copyChanged(path.join(adapters,'art-android-sources.lock'),path.join(aim,'patches/art-android/sources.lock'));
 const node=path.join(aim,'crates/aim-build/src/nodes/art.rs');let producer=fs.readFileSync(node,'utf8');
 const before='const STRIPPED: [&str; 7] = [',after='const STRIPPED: [&str; 8] = [';
 if(producer.split(before).length!==2||producer.split('    "lib64/libadbconnection.so",\n').length!==2)throw Error('Original ART plugin producer boundary changed.');
 producer=producer.replace(before,after).replace('    "lib64/libadbconnection.so",\n','    "lib64/libadbconnection.so",\n    "lib64/libopenjdkjvmti.so",\n');writeChanged(node,producer);
 const file=path.join(aim,'tools/art-android/gen_build.py');
 writeChanged(file,adaptArtCompilerPaths(fs.readFileSync(file,'utf8'),{roots:source?[[source,'/src/aim']]:[]}));
}
