package maclauncher.android;
import android.app.*;
import android.content.*;
import android.content.pm.*;
import android.content.res.*;
import android.graphics.Canvas;
import android.os.*;
import android.view.*;
import dalvik.system.PathClassLoader;
import java.lang.reflect.*;
import java.nio.file.*;
import java.nio.*;
import android.system.*;
import java.util.*;
import maclauncher.android.framework.*;
/** Original Activity/PhoneWindow bootstrap over shared installed-package/display owners. */
final class ArtActivity implements AutoCloseable {
 private static final class EndLoop extends RuntimeException {}
 /** Physical event capability; Android's own MessageQueue and ViewRoot own delivery. */
 static final class PhysicalInput extends maclauncher.android.bridge.PhysicalInput {
  PhysicalInput(ParcelFileDescriptor descriptor,Runnable closeRequested)throws Exception{super(descriptor,()->{if(closeRequested!=null)closeRequested.run();throw new EndLoop();});}
 }

 HostWindowService windows;View decor;
 final boolean originalHardwareRenderer;final LocalProviders providers=new LocalProviders();
 Activity activity;
 HostActivity task;
 boolean closed;
 AutoCloseable providerOperation;
 ArtWebKit browser;
 final Runnable closeRequested;
 private static void set(Object owner,Class<?> type,String name,Object value)throws Exception{Field f=type.getDeclaredField(name);f.setAccessible(true);f.set(owner,value);}
 private static void processRecord(ApplicationInfo info)throws Exception{
  Class<?> runtime=Class.forName("dalvik.system.VMRuntime",false,ArtActivity.class.getClassLoader());
  try{
   runtime.getDeclaredMethod("setProcessPackageName",String.class).invoke(null,info.packageName);
   runtime.getDeclaredMethod("setProcessDataDirectory",String.class).invoke(null,info.dataDir);
  }catch(InvocationTargetException failure){
   Throwable cause=failure.getCause();if(cause instanceof Exception error)throw error;if(cause instanceof Error error)throw error;throw failure;
  }
 }
 private static List<Path> compatibilityDirectories()throws Exception{
  var result=new ArrayList<Path>();result.add(Path.of("/system/etc/compatconfig"));result.add(Path.of("/system_ext/etc/compatconfig"));
  try(var input=Files.newInputStream(Path.of("/apex/apex-info-list.xml"))){
   var active=com.android.apex.XmlParser.readApexInfoList(input);
   for(var apex:active.getApexInfo())if(apex.getIsActive())result.add(Path.of("/apex",apex.getModuleName(),"etc/compatconfig"));
  }
  return result;
 }
 ArtActivity(String path,double[] display,org.json.JSONObject launch,ProcessAuthority authority)throws Exception{
  closeRequested=authority==null?null:authority::windowCloseRequested;
  originalHardwareRenderer=true;try{String packageName=launch.getString("packageName"),nativeLibraryDir=launch.optString("nativeLibraryDir","/data/app/native");
  if(Looper.myLooper()==null)Looper.prepareMainLooper();
  Path source=Path.of(path),data=Path.of("/data/user/0/"+packageName);Files.createDirectories(data.resolve("files"));Files.createDirectories(data.resolve("cache"));
  System.out.println("Activity bootstrap: private filesystem");
  HostFilesystem.configureAndroid(data,data.resolve("files"),data.resolve("cache"),packageName);
  browser=new ArtWebKit(launch.getString("browserSocket"),launch.getString("browserCapability"));
  HostContextServices.configureBrowser(data.resolve("files"),launch.optInt("networkAllowed",0)==1,launch.optInt("webLinksAllowed",0)==1,Path.of(launch.getString("browserProviderSource")));
  System.out.println("Activity bootstrap: package identity");
  HostContext.hostPackage(packageName,launch.optString("versionName",""),launch.optInt("versionCode",0));
  HostContext.hostApplicationInfo(ApplicationInfo.FLAG_HAS_CODE,path,data.toString(),nativeLibraryDir,36);
  HostContext.hostApplicationNativeAbi(launch.optString("nativeAbi",null));
  HostContext.hostApplicationIdentity(android.os.Process.myUid(),0);
  set(null,ServiceManager.class,"sServiceManager",HostServices.manager());
  System.out.println("Activity bootstrap: framework resources");
  List<Path> sources=new ArrayList<>();var sourceNames=launch.optJSONArray("sourceApks");
  if(sourceNames==null)sources.add(source);else for(int i=0;i<sourceNames.length();i++)sources.add(Path.of(sourceNames.getString(i)));
  FrameworkResources.configure(Path.of("/system/framework/framework-res.apk"),sources);
  System.out.println("Activity bootstrap: original package inspection");
  var providerInfo=HostPackages.inspectSources(sources);
  HostPackages.inspectInstalled(launch.optString("installedPackagesJson",null));
  String processName=launch.optString("processName",packageName);
  if(!HostPackages.declaredProcess(processName))throw new SecurityException("Android process is not declared by the package");
  HostContext.hostProcessName(processName);
  providerInfo=providerInfo.stream().filter(info->Objects.equals(info.processName,processName)).toList();
  ArtFilesystem.installPackageAliases(sources,HostContext.hostApplicationInfoSnapshot());
  System.out.println("Activity bootstrap: committed display");
  HostDisplay.hostPublish((long)display[0],(int)display[1],(int)display[2],(float)display[3],(float)display[4]);
  System.out.println("Activity bootstrap: original process record");
  ApplicationInfo info=HostContext.hostApplicationInfoSnapshot();
  // ActivityThread publishes these ART records before constructing an app
  // loader. An empty process directory makes primary APKs look like secondary
  // DEX files to ART's background verifier.
  processRecord(info);
  ArtLibraryCompatibility.configure(info);
  ClassLoader loader=new PathClassLoader(String.join(java.io.File.pathSeparator,sources.stream().map(Path::toString).toList()),nativeLibraryDir,ArtActivity.class.getClassLoader());
  Thread.currentThread().setContextClassLoader(loader);HostContext.hostClassLoader(loader);
  System.out.println("Activity bootstrap: shared application owner");
  var audio=new maclauncher.android.media.HostAudioManager();
  ApplicationSettings inputSettings=ApplicationSettings.read(name->launch.isNull(name)?null:launch.opt(name));
  HostContextServices.configureInputs(inputSettings);
  HostContextServices.configureMedia(inputSettings);
  Path sensorSource=launch.optBoolean("serviceProcess",false)?null:Path.of(launch.getString("sensorSource"));
  windows=HostApplication.prepare(loader,new HostContextServices.PhysicalBindings(originalHardwareRenderer,audio,
   sensorSource==null?null:()->HostSensorPublication.open(sensorSource),compatibilityDirectories()));
  Context context=HostApplication.applicationContext();context.setTheme(info.theme);
  if(authority!=null){var handler=new Handler(Looper.myLooper());authority.monitor(context,()->handler.post(()->{throw new EndLoop();}));}
  if(!launch.optBoolean("serviceProcess",false))ArtPlatform.service(Context.CONNECTIVITY_SERVICE,HostServiceEndpoints.export(HostConnectivityManager.service(),"android.net.IConnectivityManager",context));
  HostStorageService.install();
  java.util.concurrent.FutureTask<Void> nativeAudio=null;
  if(launch.optBoolean("nativeServices",false)){
   ArtPlatform.activity(HostServiceEndpoints.export(ActivityManager.getService(),"android.app.IActivityManager",context));
   ArtPlatform.audioService(maclauncher.android.media.HostAudioService.nativeEndpoint());
   int count=android.media.AudioSystem.getNumStreamTypes();int[] ranges=new int[count*3];
   for(int stream=0;stream<count;++stream){ranges[stream*3]=audio.getStreamMinVolume(stream);ranges[stream*3+1]=audio.getStreamMaxVolume(stream);ranges[stream*3+2]=audio.getStreamVolume(stream);}
   // Native services start alongside the rest of the bootstrap. Application
   // constructors may immediately open AudioTrack, so initial stream ranges
   // and the original permission replay must finish before invoking them.
   nativeAudio=new java.util.concurrent.FutureTask<>(()->{ArtPlatform.audio(ranges);System.out.println("Original native audio policy and shared stream ranges ready");return null;});
   var startup=new Thread(nativeAudio,"Android native audio startup");startup.setDaemon(true);startup.start();
   for(String name:new String[]{"appops","permission_checker"}){
    IBinder local=HostServices.manager().checkService(name);String descriptor=local.getInterfaceDescriptor();
    IInterface original=local.queryLocalInterface(descriptor);
    ArtPlatform.service(name,original==null?local:HostServiceEndpoints.export(original,descriptor,context));
   }
  }
  if(nativeAudio!=null)try{nativeAudio.get();}
  catch(java.util.concurrent.ExecutionException failure){Throwable cause=failure.getCause();if(cause instanceof Error error)throw error;if(cause instanceof Exception error)throw error;throw new IllegalStateException("Native audio initialization failed",cause);}
  System.out.println("Activity bootstrap: original Application");
  Application app=HostApplication.instantiate(loader,info.className);
  HostContextServices.applicationCreated(app,Looper.myLooper());providerOperation=HostContextServices.installProviders(app,providerInfo,providers,launch.has("googleFreeAcquisition"));HostApplication.create(app);
  if(launch.optBoolean("serviceProcess",false)){activity=null;return;}
  System.out.println("Activity bootstrap: original Activity context");
  ActivityInfo activityInfo=HostPackages.activityInfo(launch.getString("activityClass"));
  activity=HostActivity.instantiate(loader,activityInfo.name);
  HostActivity.attach(activity,context,app,activityInfo);task=HostActivity.owner(activity);activity.setTheme(activityInfo.getThemeResource());
  System.out.println("Activity bootstrap: original Activity create");
  HostActivity.start(activity,null);
  System.out.println("Original APK Activity + PhoneWindow created on shared display "+activity.getDisplay().getWidth()+"x"+activity.getDisplay().getHeight());
  }catch(Exception|Error failure){FirstFailure.reportOwner(failure);try{close();}catch(Throwable cleanup){if(cleanup!=failure)failure.addSuppressed(cleanup);}throw failure;}
 }
 void serve(){try{Looper.loop();}catch(EndLoop finished){}}
 void show(SurfaceControl output,int width,int height)throws Exception{
  var window=HostActivity.openWindow(String.valueOf(activity.getTitle()),width,height,closeRequested);Throwable failure=null;
  try{
   window.attachActivity(activity);
   try{window.present(activity,String.valueOf(activity.getTitle()),width,height);}catch(EndLoop finished){}
  }catch(RuntimeException|Error error){FirstFailure.reportOwner(error);failure=error;}
  finally{failure=HostActivity.closeWindow(failure,task::finish,window);}
  if(failure instanceof Error error)throw error;if(failure instanceof Exception error)throw error;
 }
 @Override public void close()throws Exception{
  if(closed)return;closed=true;Throwable failure=null;
  List<Runnable> retire=new ArrayList<>();
  if(providerOperation!=null)retire.add(()->{try{providerOperation.close();}catch(RuntimeException|Error error){throw error;}catch(Exception error){throw new IllegalStateException("Provider preference lease could not close",error);}});
  if(task!=null)retire.add(task::finish);
  // Engines can wait for an actual final frame in onPause/onStop. Keep original
  // SurfaceFlinger and the physical display clock live through those callbacks.
  retire.add(HostWindowManager::shutdown);
  retire.add(HostRemoteServices::shutdown);
  for(Runnable operation:retire)try{operation.run();}catch(Throwable error){if(failure==null)failure=error;else if(failure!=error)failure.addSuppressed(error);}
  failure=HostContextServices.retireApplication(failure,providers);
  failure=HostContextServices.retireContext(failure,Looper.myLooper());
  if(browser!=null)try{browser.close();}catch(Throwable error){if(failure==null)failure=error;else if(failure!=error)failure.addSuppressed(error);}
  if(failure instanceof Error error)throw error;if(failure instanceof Exception error)throw error;
 }
}
