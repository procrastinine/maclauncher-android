package maclauncher.android;

import android.media.IAudioService;
import android.os.*;
import java.nio.file.*;
import maclauncher.android.framework.*;
import org.json.JSONObject;

/** ART entry over original DEX/framework and the shared application owners. */
public final class Art {
 private Art(){}
 public static void main(String[] args){
  int status=0;
  try(var workers=FirstFailure.captureWorkers()){
   try{if(args.length!=2||!"--config".equals(args[0]))throw new IllegalArgumentException("Expected --config FILE");run(new JSONObject(new String(Files.readAllBytes(Path.of(args[1])),java.nio.charset.StandardCharsets.UTF_8)));}
   catch(Throwable failure){FirstFailure.reportOwner(failure);status=1;}
  }
  System.exit(status);
 }
 static void run(JSONObject launch)throws Exception{
  ArtResourceInitialization.prepare(launch.getInt("runtimeUid"));
  System.setProperty("klepton.bridge","/system/lib64/libart-platform.so");
  System.setProperty("klepton.graphics","/system/lib64/libart-platform.so");
  System.setProperty("klepton.audio","/system/lib64/libart-platform.so");
  System.load("/system/lib64/libart-authority.so");
  System.load("/system/lib64/libart-rpc.so");
  System.load("/system/lib64/libart-platform.so");
  HostDigests.initialize();
  HostInput.keyboardLoader(ArtPlatform::keyboard);
  String packageName=launch.getString("packageName");
  HostAndroidIdentity.prepare(Path.of(launch.getString("filesRoot")).getParent());
  HostGoogleProviders.configure(launch.optInt("googleProviderCompatibility",0)==1);
  boolean networkAllowed=launch.optInt("networkAllowed",0)==1;
  maclauncher.android.net.HostNetworkPolicy.configure(launch.optInt("allowAds",0)==1);
  if(launch.optBoolean("serviceProcess",false))HostConnectivityManager.configure(networkAllowed,android.net.IConnectivityManager.Stub.asInterface(ArtPlatform.connectivity()));
  else HostConnectivityManager.configure(networkAllowed,new ArtNetwork());
  try(ProcessAuthority authority=ProcessAuthority.read(5,packageName,launch.optString("processName",packageName))){
   if(authority.self.uid()!=android.os.Process.myUid()||authority.self.pid()!=android.os.Process.myPid())throw new SecurityException("Android process identity differs from enrollment");
   ArtPlatform.initializeRpc(authority.self.hostUid());
   authority.attachPlatform();
   if(!launch.optBoolean("serviceProcess",false))HostRemoteServices.prepareWindowServices();
   var audio=IAudioService.Stub.asInterface(HostServices.manager().checkService("audio"));
   if(audio!=null)maclauncher.android.media.HostAudioService.nativePermissionBarrier(()->{try{audio.permissionUpdateBarrier();}catch(RemoteException failure){throw failure.rethrowFromSystemServer();}});
   JSONObject display=launch.getJSONObject("display");double[] dimensions={display.getLong("screen"),display.getInt("width"),display.getInt("height"),display.getDouble("refresh"),display.getInt("densityDpi")/160.0};
   var output=launch.optBoolean("serviceProcess",false)?null:ArtPlatform.output();
   ArtWindows.install(output,launch.optBoolean("serviceProcess",false));
   try(ArtActivity activity=new ArtActivity(launch.getString("sourceApk"),dimensions,launch,authority)){if(output==null)activity.serve();else activity.show(output,(int)dimensions[1],(int)dimensions[2]);}
   finally{if(output!=null)output.release();}
  }
 }
}
