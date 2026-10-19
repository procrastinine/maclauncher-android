package maclauncher.android;

import android.view.SurfaceControl;
import java.util.concurrent.CountDownLatch;
import maclauncher.android.bridge.NativeComposition;
import maclauncher.android.framework.HostBinderRpc;
import maclauncher.android.framework.HostProcessServices;

/** Original native layer owner inside the launcher's existing graphics session.
 * ProcessAuthority supplies enrollment; the endpoint itself grants no PIDs. */
public final class NativeCompositionHost {
 private NativeCompositionHost(){}
 public static void main(String[] args)throws Exception{
  if(args.length!=2)throw new IllegalArgumentException("Expected native composition socket and library");
  System.setProperty("klepton.bridge","/system/lib64/libart-platform.so");
  System.load("/system/lib64/libart-authority.so");System.load("/system/lib64/libart-rpc.so");System.load("/system/lib64/libart-platform.so");
  try(var authority=ProcessAuthority.read(5,"android","android.composition");var processes=new HostProcessServices()){
   if(authority.self.uid()!=1000||authority.self.uid()!=android.os.Process.myUid()||!authority.self.securityContext().equals("u:r:system_server:s0"))throw new SecurityException("Native composition has an invalid enrolled identity");
   ArtPlatform.initializeRpc(authority.self.hostUid());
   var root=new SurfaceControl.Builder().setName("Android application composition").setContainerLayer().build();
   try{
    try(var transaction=new SurfaceControl.Transaction()){transaction.setLayerStack(root,0).show(root).apply();}
    var stopped=new CountDownLatch(1);
    try(var composition=NativeComposition.serve(root,args[1]);var listener=HostBinderRpc.listen(args[0],composition,processes,4)){
     authority.monitorPlatform(processes,stopped::countDown);stopped.await();
    }
   }finally{try(var transaction=new SurfaceControl.Transaction()){transaction.remove(root).apply();}root.release();}
  }
 }
}
