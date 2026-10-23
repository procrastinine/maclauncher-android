package com.android.server.pm.dex;

import android.content.pm.IPackageManager;
import android.util.AtomicFile;
import java.io.IOException;
import java.nio.file.*;
import java.lang.reflect.Field;
import java.util.Map;

/** The original dex manager and stats algorithms with installation-owned files. */
public final class HostDexReports extends DexManager {
 private static final class Usage extends PackageDexUsage {
  private final AtomicFile file;
  Usage(Path root){file=new AtomicFile(root.resolve("package-dex-usage.list").toFile());}
  @Override protected AtomicFile getFile(){return file;}
 }
 private static final class Loading extends PackageDynamicCodeLoading {
  private final AtomicFile file;
  Loading(Path root){file=new AtomicFile(root.resolve("package-dcl.list").toFile());}
  @Override protected AtomicFile getFile(){return file;}
 }
 private final DynamicCodeLogger dynamic;
 public HostDexReports(Path directory,IPackageManager packages)throws IOException {
  this(storage(directory,packages),packages);
 }
 private record Storage(Usage usage,DynamicCodeLogger dynamic){}
 private static Storage storage(Path directory,IPackageManager packages)throws IOException{
  Path root=Files.createDirectories(directory).toRealPath();return new Storage(new Usage(root),new DynamicCodeLogger(packages,null,new Loading(root)));
 }
 private HostDexReports(Storage storage,IPackageManager packages){
  super(null,null,storage.dynamic(),packages);this.dynamic=storage.dynamic();
  // The upstream constructor owns this record privately. Replace only its
  // AtomicFile boundary, retaining every original record/read/write algorithm.
  try{Field field=DexManager.class.getDeclaredField("mPackageDexUsage");field.setAccessible(true);field.set(this,storage.usage());}
  catch(ReflectiveOperationException failure){throw new IllegalStateException("Android dex usage storage is unavailable",failure);}
 }
 public void retirePackage(String packageName){
  try{
   Field field=DexManager.class.getDeclaredField("mPackageCodeLocationsCache");field.setAccessible(true);
   Map<?,?> locations=(Map<?,?>)field.get(this);synchronized(locations){locations.remove(packageName);}
  }catch(ReflectiveOperationException failure){throw new IllegalStateException("Android dex package locations are unavailable",failure);}
  notifyPackageDataDestroyed(packageName,android.os.UserHandle.USER_ALL);
  dynamic.notifyPackageDataDestroyed(packageName,android.os.UserHandle.USER_ALL);
 }
 public void writeNow(){writePackageDexUsageNow();dynamic.writeNow();}
}
