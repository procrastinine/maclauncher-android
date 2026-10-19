import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {writeFrameworkNativeCatalog} from './framework-native-catalog.mjs';
import {writeKeyStoreDeclarations} from './keystore/declarations.mjs';
import {prepareSourceTree} from './upstream/source-files.mjs';

// Full and incremental builds assemble the same host archive, including the
// native publisher catalog and JVM service-provider resources.
export function buildHostArchive({root, output, build, hostClasses, frameworkTool, frameworkInput, asmClasspath, java, jar}) {
  const run=(command,args)=>execFileSync(command,args,{stdio:'inherit'});
  const assembly=path.join(build,'host-archive');
  prepareSourceTree(assembly,temporary=>{
    fs.cpSync(hostClasses,temporary,{recursive:true,mode:fs.constants.COPYFILE_FICLONE});
    const resources=path.join(temporary,'maclauncher/android/framework');
    fs.mkdirSync(resources,{recursive:true});
    fs.copyFileSync(path.join(root,'resources/Virtual.kcm'),path.join(resources,'Virtual.kcm'));
    fs.copyFileSync(path.join(root,'resources/storage-namespace.json'),path.join(resources,'storage-namespace.json'));
    const services=path.join(temporary,'META-INF/services');
    fs.mkdirSync(services,{recursive:true});
    fs.writeFileSync(path.join(services,'java.net.spi.InetAddressResolverProvider'),'maclauncher.android.net.HostInetAddressResolverProvider\n');
    const declarations=path.join(build,'framework-native-declarations.tsv');
    run(java,['-cp',[frameworkTool,asmClasspath].join(path.delimiter),'FrameworkNativeDeclarations',frameworkInput,path.join(output,'android-framework.jar'),declarations]);
    writeFrameworkNativeCatalog(path.join(output,'native/libandroid_runtime.dylib'),path.join(resources,'native-families.properties'),declarations);
    writeKeyStoreDeclarations({root,build,output:path.join(resources,'keystore-services.properties')});
  });
  run(jar,['--create','--file',path.join(output,'android-host.jar'),'--date','1980-01-01T00:00:02Z','-C',assembly,'.']);
}
