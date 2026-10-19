package maclauncher.android;

import android.os.Looper;
import java.nio.file.*;
import java.util.List;
import maclauncher.android.framework.*;

/** The profile-owned process publishes the original Android platform services. */
public final class PlatformServiceHost {
    public static void main(String[] args) {
        int status=0;
        try(var failures=FirstFailure.captureWorkers()) {
            try{run(args);}catch(Throwable failure){FirstFailure.reportOwner(failure);status=1;}
        }
        System.exit(status);
    }
    private static void run(String[] args)throws Throwable {
        if(args.length!=2||!"--config".equals(args[0]))throw new IllegalArgumentException("Use --config followed by the Android platform configuration");
        var config=PlatformServiceConfig.read(Path.of(args[1]));
        RuntimeBootstrap.initialize(config.runtimeRoot(),config.dataRoot(),config.filesRoot(),config.cacheRoot(),config.packageName(),false,false,false);
        try(var ownership=android.os.ParcelFileDescriptor.adoptFd(config.ownershipFd());
            var authority=ProcessAuthority.read(config.authorityFd(),config.packageName(),config.processName())) {
            if(authority.self.uid()!=config.uid()||!config.securityContext().equals(authority.self.securityContext()))throw new SecurityException("Android platform service requires its authenticated identity");
            HostProcessIdentity.configure(authority.self.uid(),authority.self.hostUid());
            HostFilesystem.installNativeMappings(List.of(),config.filesRoot());
            Looper looper=HostLooper.main();
            try(var processes=new HostProcessServices();
                var system=config.role().equals("system")?HostSystemServices.start(config.runtimeRoot(),config.dataRoot(),config.processName()):null;
                var processInfo=system==null?null:new HostProcessInfo(HostApplication.applicationContext(),processes);
                var permissions=system==null?null:HostPermissions.remote(processes)) {
                if(config.role().equals("keystore"))HostKeyStore.start(config.runtimeRoot(),config.filesRoot());
                try(var directory=system==null?null:new HostServiceDirectory(HostApplication.applicationContext(),true,processes);
                    var listener=HostBinderRpc.listen(config.platformSocketPath().toString(),directory==null?HostServices.publishedManager().asBinder():directory.root(),processes,4)) {
                    authority.monitorPlatform(processes,()->new android.os.Handler(looper).post(()->HostLooper.close(looper)),system!=null);
                    try{Looper.loop();}finally{listener.close();listener.closed().toCompletableFuture().join();}
                }
            }finally{if(config.role().equals("system"))HostApplication.shutdown();HostLooper.close(looper);}
        }
    }
}
