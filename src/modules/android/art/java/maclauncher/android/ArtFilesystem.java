package maclauncher.android;

import android.content.pm.ApplicationInfo;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.List;
import maclauncher.android.framework.HostFilesystem;

/** Publish shared package paths through the original Linux mount owner. */
final class ArtFilesystem {
 private ArtFilesystem(){}
 static void installPackageAliases(List<Path> sources,ApplicationInfo info)throws IOException{
  for(var alias:HostFilesystem.fusedArchivePaths(sources,info).entrySet()){
   byte[] name=alias.getKey().toString().getBytes(StandardCharsets.UTF_8),target=alias.getValue().toString().getBytes(StandardCharsets.UTF_8);
   byte[] packet=ByteBuffer.allocate(8+name.length+target.length).order(ByteOrder.LITTLE_ENDIAN)
     .putInt(name.length).putInt(target.length).put(name).put(target).array();
   int result=bindPackagePath(packet);
   if(result<0)throw new IOException("Android package path publication failed: errno="+(-result));
  }
 }
 private static native int bindPackagePath(byte[] packet);
}
