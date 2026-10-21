package maclauncher.android.framework;

import android.Manifest;
import android.app.*;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.*;
import android.permission.PermissionManager;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** Runs the original ContextImpl and permission clients against the actual process owner. */
public final class ActivityServiceBootstrapProbe {
    private static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    private static void permissions(){
        int pid=android.os.Process.myPid(),uid=android.os.Process.myUid();
        check(PermissionManager.checkPermission(Manifest.permission.INTERNET,pid,uid,Context.DEVICE_ID_DEFAULT)==PackageManager.PERMISSION_GRANTED,"original normal manifest permission is granted");
        for(String name:List.of(Manifest.permission.STATUS_BAR_SERVICE,Manifest.permission.CAMERA,Manifest.permission.POST_NOTIFICATIONS))
            check(PermissionManager.checkPermission(name,pid,uid,Context.DEVICE_ID_DEFAULT)==PackageManager.PERMISSION_DENIED,"ungranted signature/sensitive permission remains denied: "+name);
        check(PermissionManager.checkPermission(Manifest.permission.INTERNET,pid,uid+1,Context.DEVICE_ID_DEFAULT)==PackageManager.PERMISSION_DENIED,"another UID does not borrow this manifest grant");
        check(PermissionManager.checkPermission(Manifest.permission.INTERNET,pid,uid,Context.DEVICE_ID_DEFAULT+1)==PackageManager.PERMISSION_GRANTED,"normal manifest grants retain their existing device-independent policy");
    }
    public static void main(String[] args)throws Exception{
        Path runtime=Path.of(args[0]),data=Files.createDirectories(Path.of(args[1])),manifest=Path.of(args[2]);String mode=args[3];
        Path files=Files.createDirectories(data.resolve("files")),cache=Files.createDirectories(data.resolve("cache")),apk=data.resolve("application.apk");
        try(var output=new ZipOutputStream(Files.newOutputStream(apk))){output.putNextEntry(new ZipEntry("AndroidManifest.xml"));Files.copy(manifest,output);output.closeEntry();}
        var bootstrap=Class.forName("maclauncher.android.RuntimeBootstrap").getDeclaredMethod("initialize",Path.class,Path.class,Path.class,Path.class,String.class,boolean.class,boolean.class,boolean.class);
        bootstrap.setAccessible(true);bootstrap.invoke(null,runtime,data,files,cache,"org.example.permissions",false,false,false);
        System.load(System.getProperty("klepton.graphics"));
        Looper loop=HostLooper.main();
        FrameworkResources.configure(runtime.resolve("android-framework.jar"),List.of(apk));
        HostContext.hostPackage("org.example.permissions","1",1);HostContext.hostApplicationInfo(false,apk.toString(),data.toString(),runtime.resolve("native").toString(),35);
        HostContext.hostApplicationIdentity(android.os.Process.myUid(),0);HostPackages.inspectSources(List.of(apk));
        HostDisplay.hostPublish(1,320,240,60);HostStorageService.install();
        if(mode.equals("precontext")){
            check(ActivityThread.currentActivityThread()==null,"no application Context/ActivityThread is bound");
            var owner=new HostActivityService();
            try{
                var endpoint=HostServices.manager().checkService(Context.ACTIVITY_SERVICE);
                check(endpoint!=null&&endpoint.queryLocalInterface(IActivityManager.class.getName())==owner,"one actual activity endpoint is published");
                check(ActivityManager.getService()!=null,"the original ActivityManager client sees that endpoint");
                PermissionManager.disablePermissionCache();permissions();
            }finally{owner.close();}
            owner.close();
            check(HostServices.manager().checkService(Context.ACTIVITY_SERVICE)==null&&ActivityManager.getService()==null,"pre-Context retirement removes both directory and original singleton client");
            HostContextServices.retireContext(null,loop);HostLooper.close(loop);
            System.out.println("PASS pre-Context original permission checks and idempotent endpoint retirement");System.exit(0);return;
        }
        check(mode.equals("bootstrap"),"known probe mode");
        try{
            HostApplication.prepare(ActivityServiceBootstrapProbe.class.getClassLoader());
            Context context=HostApplication.applicationContext();
            check(context.getClass().getName().equals("android.app.ContextImpl"),"the actual original ContextImpl is retained");
            var service=ActivityManager.getService();check(service!=null,"original ActivityManager client is available after first Context creation");
            check(context.checkSelfPermission(Manifest.permission.INTERNET)==PackageManager.PERMISSION_GRANTED,"original Context permission check keeps the normal grant");
            permissions();
            Application application=HostApplication.instantiate(ActivityServiceBootstrapProbe.class.getClassLoader(),null);HostApplication.create(application);
            check(application.getBaseContext()==context&&ActivityManager.getService()==service,"original Application attachment retains the same Context and activity client");
        }finally{
            Throwable failure=HostContextServices.retireApplication(null,null);
            check(HostServices.manager().checkService(Context.ACTIVITY_SERVICE)==null,"normal retirement removes its actual directory endpoint");
            failure=HostContextServices.retireContext(failure,loop);HostLooper.close(loop);
            check(ActivityManager.getService()==null,"Looper retirement removes the original cached activity client");
            if(failure!=null)throw new AssertionError("actual bootstrap retirement failed",failure);
        }
        System.out.println("PASS original ContextImpl first permission lookup, Application attachment and activity retirement");System.exit(0);
    }
}
