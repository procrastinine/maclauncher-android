package maclauncher.android;

import android.content.pm.ApplicationInfo;
import com.android.internal.content.NativeLibraryHelper;
import com.android.server.pm.parsing.pkg.AndroidPackageUtils;
import maclauncher.android.framework.HostPackages;

/** Original installation alignment policy and Bionic's per-process loader mode. */
final class ArtLibraryCompatibility {
 private ArtLibraryCompatibility(){}
 static void configure(ApplicationInfo info)throws java.io.IOException{
  int alignment=ApplicationInfo.PAGE_SIZE_APP_COMPAT_FLAG_UNDEFINED;
  if("arm64-v8a".equals(info.primaryCpuAbi)){
   var state=HostPackages.hostGetPackageStateInternal(null,info.packageName);
   try(var handle=AndroidPackageUtils.createNativeLibraryHandle(state.getPkg())){
    alignment=NativeLibraryHelper.checkAlignmentForCompatMode(handle,info.nativeLibraryDir,false,info.primaryCpuAbi);
   }
  }
  ArtPlatform.libraryCompatibility(info.targetSdkVersion,HostPackages.hostPageSizeCompatibility(alignment));
 }
}
