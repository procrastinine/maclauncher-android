package maclauncher.android.framework;

import android.app.*;
import android.content.*;
import android.content.res.Configuration;
import android.os.*;
import com.android.server.locales.HostLocaleService;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;
import java.util.concurrent.TimeUnit;
import maclauncher.android.fixture.RuntimeFixture;

/** Complete original locale Binder service with real storage, permissions and callbacks. */
public final class LocaleServiceProbe {
 static void check(boolean value,String text){if(!value)throw new AssertionError(text);}
 interface Operation{void run()throws Exception;}
 static void denied(Operation operation)throws Exception{try{operation.run();throw new AssertionError("Missing original permission rejection");}catch(SecurityException expected){}}
 static void await(Looper loop,java.util.function.BooleanSupplier state)throws Exception{long end=System.nanoTime()+TimeUnit.SECONDS.toNanos(4);while(!state.getAsBoolean()){HostLooper.drain(loop);if(System.nanoTime()>end)throw new AssertionError("Configuration notification did not arrive");Thread.sleep(5);}}
 static String tags(Context context){return context.getResources().getConfiguration().getLocales().toLanguageTags();}
 static IRemoteCallback callback(Context context,HostBinderRpc.Peer owner)throws Exception{Class<?> type=Class.forName("maclauncher.android.framework.HostLocaleConfiguration$Client");var constructor=type.getDeclaredConstructor(Context.class,HostBinderRpc.Peer.class);constructor.setAccessible(true);return (IRemoteCallback)constructor.newInstance(context,owner);}
 static native void imeQuery(boolean enabled);
 static android.content.SharedPreferences delegateInfo(HostLocaleService owner)throws Exception{var field=HostLocaleService.class.getDeclaredField("backup");field.setAccessible(true);Object backup=field.get(owner);var method=backup.getClass().getMethod("getPersistedInfo");method.setAccessible(true);return (android.content.SharedPreferences)method.invoke(backup);}
 static void closeCallback(IRemoteCallback callback)throws Exception{var method=callback.getClass().getDeclaredMethod("close");method.setAccessible(true);method.invoke(callback);}
 static void rejectQueuedCallback(IRemoteCallback callback,HostBinderRpc.Peer app,HostBinderRpc.Peer system,Context context,Looper loop)throws Exception{
  var pair=ParcelFileDescriptor.createSocketPair();String before=tags(context);
  var field=callback.getClass().getDeclaredField("generation");field.setAccessible(true);String generation=(String)field.get(callback);
  try(var server=HostBinderRpc.serve(pair[0],system,callback.asBinder(),2);var connection=HostBinderRpc.connect(pair[1],app,2)){
   Bundle result=new Bundle();result.putString("package",app.packageName());result.putInt("user",0);result.putString("generation",generation);result.putLong("sequence",Long.MAX_VALUE);result.putParcelable("locales",LocaleList.forLanguageTags("it-IT"));
   IRemoteCallback.Stub.asInterface(connection.root()).sendResult(result);closeCallback(callback);HostLooper.drain(loop);check(tags(context).equals(before),"A callback queued before retirement cannot alter resources afterward");
  }finally{pair[0].close();pair[1].close();}
 }
 static void run(String[] args)throws Exception{
  Path runtime=Path.of(args[0]),data=Files.createDirectories(Path.of(args[1])),apk=data.resolve("app.apk");
  try(var output=new ZipOutputStream(Files.newOutputStream(apk))){output.putNextEntry(new ZipEntry("AndroidManifest.xml"));Files.copy(Path.of(args[2]),output);output.closeEntry();}
  Looper loop=RuntimeFixture.initialize(runtime.toString(),data.toString());System.load(args[3]);
  // No Android IME/Settings provider is started by this isolated service check.
  // Select the original optional IME-query policy through its real property owner.
  imeQuery(false);
  HostProcessIdentity.configure(1000,(int)new com.sun.security.auth.module.UnixSystem().getUid());
  FrameworkResources.configure(runtime.resolve("android-framework.jar"),List.of(apk));HostDisplay.hostPublish(1,320,240,60);
  HostContext.hostPackage("org.example.permissions","1",1);HostContext.hostApplicationInfo(false,apk.toString(),data.toString(),runtime.resolve("native").toString(),35);HostContext.hostApplicationIdentity(20001,0);HostPackages.inspectSources(List.of(apk));HostStorageService.install();
  HostApplication.prepare(LocaleServiceProbe.class.getClassLoader());Context context=HostApplication.applicationContext();Context serviceContext=context.createAttributionContext(null);LocaleList systemLocales=ActivityManager.getService().getConfiguration().getLocales();
  var app=new HostBinderRpc.Peer(context.getPackageName(),context.getPackageName(),android.os.Process.myPid(),20001);
  var system=new HostBinderRpc.Peer("android","android.system",android.os.Process.myPid(),1000);
  LocaleList selected=LocaleList.forLanguageTags("fr-FR,de-DE"),replaced=LocaleList.forLanguageTags("de-DE");
  try{
   for(int phase=0;phase<2;phase++)try(var owner=new HostLocaleService(serviceContext)){
    var original=owner.endpoint();for(var method:ILocaleManager.class.getDeclaredMethods())check(original.getClass().getMethod(method.getName(),method.getParameterTypes()).getDeclaringClass().getName().equals("com.android.server.locales.LocaleManagerService$LocaleManagerBinderService"),"Complete original Binder method retained: "+method);
    if(phase==1){check(original.getApplicationLocales(app.packageName(),0).equals(replaced),"Original package XML survives service reconstruction");check(original.getOverrideLocaleConfig(app.packageName(),0).getSupportedLocales().equals(replaced),"Original supported-locale XML survives reconstruction");check(delegateInfo(owner).getStringSet("0",Set.of()).contains(app.packageName()),"Original backup delegate preferences survive reconstruction");check(Files.isRegularFile(Environment.getDataSystemDeDirectory(0).toPath().resolve("LocalesFromDelegatePrefs.xml")),"Delegate preferences use the original real XML file");original.setApplicationLocales(app.packageName(),0,LocaleList.getEmptyLocaleList(),false);owner.packageChanged(app.packageName(),app.uid());owner.packageDataCleared(app.packageName(),app.uid());var drained=new java.util.concurrent.CountDownLatch(1);var handlerField=HostLocaleService.class.getDeclaredField("thread");handlerField.setAccessible(true);((HandlerThread)handlerField.get(owner)).getThreadHandler().post(drained::countDown);check(drained.await(4,TimeUnit.SECONDS),"Original package monitor events drain on their service handler");check(com.android.server.wm.HostPackageConfiguration.get(null,app.packageName(),0)==null,"Original package-data-clear event retires package configuration");continue;}
    var callback=callback(context,system);HostServices.register("maclauncher.package_configuration",callback,"android.os.IRemoteCallback");
    var pair=ParcelFileDescriptor.createSocketPair();
    try(var processes=new HostProcessServices();var directory=new HostServiceDirectory(context,true,processes);var appDirectory=new HostServiceDirectory(context);
        var server=HostBinderRpc.serve(pair[0],app,directory.root(),4);var connection=HostBinderRpc.connect(pair[1],system,4);
        var notifications=HostLocaleConfiguration.remote(processes)){
      processes.stage(server);HostServiceDirectory.publish(connection.root(),appDirectory.root());
      var manager=IServiceManager.Stub.asInterface(connection.root());var remote=ILocaleManager.Stub.asInterface(manager.getService(Context.LOCALE_SERVICE));
      check(remote!=null&&remote.getApplicationLocales(app.packageName(),0).isEmpty(),"Original empty locale state");check(remote.getSystemLocales().equals(context.getResources().getConfiguration().getLocales()),"Original system locales use real configuration");
      remote.setApplicationLocales(app.packageName(),0,selected,true);check(remote.getApplicationLocales(app.packageName(),0).equals(selected),"Own-package Binder get/set");
      await(loop,()->tags(context).startsWith("fr-FR,de-DE"));check(HostUiAccess.get(ActivityThread.currentActivityThread(),ActivityThread.class,"mInitialApplication")==null,"Package configuration applies before Application attachment");HostApplication.attach(new Application(),context);check(ActivityManager.getService().getConfiguration().getLocales().equals(systemLocales),"ActivityManager retains global locales after the app overlay");check(remote.getSystemLocales().equals(systemLocales),"LocaleManager system locales remain global after app configuration");check(delegateInfo(owner).getStringSet("0",Set.of()).contains(app.packageName()),"Original backup delegate metadata is persisted");byte[] backup=com.android.server.LocalServices.getService(com.android.server.locales.LocaleManagerInternal.class).getBackupPayload(0);check(backup!=null&&new String(backup,java.nio.charset.StandardCharsets.UTF_8).contains("fr-FR,de-DE"),"Original backup API serializes the real locale state");check(!HostActivityManager.hostIsAppForeground(null,20002),"An absent UID is not a foreground IME exception");check(HostPackageConfigurationRecord(app.packageName()).equals(selected),"Original package configuration owner");
      denied(()->remote.getApplicationLocales(app.packageName(),10));denied(()->remote.setApplicationLocales(app.packageName(),10,selected,false));denied(()->remote.setOverrideLocaleConfig(app.packageName(),10,new LocaleConfig(selected)));
      var foreignPair=ParcelFileDescriptor.createSocketPair();var foreign=new HostBinderRpc.Peer("fixture.foreign","fixture.foreign",android.os.Process.myPid(),20002);
      try(var foreignServer=HostBinderRpc.serve(foreignPair[0],foreign,original.asBinder(),2);var foreignClient=HostBinderRpc.connect(foreignPair[1],system,2)){
        var untrusted=ILocaleManager.Stub.asInterface(foreignClient.root());denied(()->untrusted.getApplicationLocales(app.packageName(),0));denied(()->untrusted.setApplicationLocales(app.packageName(),0,selected,false));denied(()->untrusted.setOverrideLocaleConfig(app.packageName(),0,new LocaleConfig(selected)));
      }finally{foreignPair[0].close();foreignPair[1].close();}
      remote.setOverrideLocaleConfig(app.packageName(),0,new LocaleConfig(replaced));check(remote.getOverrideLocaleConfig(app.packageName(),0).getSupportedLocales().equals(replaced),"Original override config roundtrip");check(remote.getApplicationLocales(app.packageName(),0).equals(replaced),"Original override removes unsupported app locales");await(loop,()->tags(context).startsWith("de-DE"));
      denied(()->callback.sendResult(new Bundle()));
      processes.unavailable(app);original.setApplicationLocales(app.packageName(),0,selected,false);HostLooper.drain(loop);check(tags(context).startsWith("de-DE"),"Retired process directory receives no configuration");original.setApplicationLocales(app.packageName(),0,replaced,true);rejectQueuedCallback(callback,app,system,context,loop);
    }finally{pair[0].close();pair[1].close();HostServices.unregister("maclauncher.package_configuration",callback,"android.os.IRemoteCallback");}
   }
   check(HostServices.manager().checkService(Context.LOCALE_SERVICE)==null,"Locale Binder owner retired");check(Thread.getAllStackTraces().keySet().stream().noneMatch(t->t.isAlive()&&t.getName().equals("Android locales")),"Original locale owner thread retired");
  }finally{HostApplication.shutdown();HostLooper.close(loop);FrameworkResources.shutdown();}
  System.out.println("PASS full original LocaleManager: Binder get/set, actual configuration, original persistence/delegate backup/override filtering, foreign package/user rejection, pre-Application notification, global configuration and retirement");
 }
 static LocaleList HostPackageConfigurationRecord(String name){return com.android.server.wm.HostPackageConfiguration.get(null,name,0).mLocales;}
 public static void main(String[] args){try{run(args);System.exit(0);}catch(Throwable failure){failure.printStackTrace();System.exit(1);}}
}
