package maclauncher.android;

import android.content.pm.ApplicationInfo;
import com.android.internal.content.NativeLibraryHelper;
import com.android.server.pm.parsing.pkg.AndroidPackageUtils;
import maclauncher.android.framework.HostPackages;

/** Original installation alignment policy and Bionic's per-process loader mode. */
final class ArtLibraryCompatibility {
 private ArtLibraryCompatibility(){}
 private static int alignment(com.android.server.pm.pkg.PackageStateInternal state,String directory,String abi)throws java.io.IOException{
  if(state==null||state.getPkg()==null||!"arm64-v8a".equals(abi))return ApplicationInfo.PAGE_SIZE_APP_COMPAT_FLAG_UNDEFINED;
  try(var handle=AndroidPackageUtils.createNativeLibraryHandle(state.getPkg())){
   return NativeLibraryHelper.checkAlignmentForCompatMode(handle,directory,false,abi);
  }
 }
 static void configure(ApplicationInfo info)throws java.io.IOException{
  int flags=alignment(HostPackages.hostGetPackageStateInternal(null,info.packageName),info.nativeLibraryDir,info.primaryCpuAbi);
  if(flags==ApplicationInfo.PAGE_SIZE_APP_COMPAT_FLAG_ERROR)flags=ApplicationInfo.PAGE_SIZE_APP_COMPAT_FLAG_UNDEFINED;
  // This is the process's authenticated code catalog, whose APKs and native
  // directories are already mounted read-only. A provider can supply native
  // code to an otherwise aligned app through its original Context/ClassLoader.
  for(var state:HostPackages.hostGetPackageStates(null).values()){
   if(state.getPackageName().equals(info.packageName)||state.getPkg()==null)continue;
   int peer=alignment(state,state.getPkg().getNativeLibraryDir(),state.getPrimaryCpuAbi());
   if(peer!=ApplicationInfo.PAGE_SIZE_APP_COMPAT_FLAG_ERROR)flags|=peer;
  }
  // The original app's explicit manifest/settings disable remains decisive.
  ArtPlatform.libraryCompatibility(info.targetSdkVersion,HostPackages.hostPageSizeCompatibility(flags));
 }
}
