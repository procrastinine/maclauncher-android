package maclauncher.android;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.nio.*;
import java.nio.channels.FileChannel;
import java.io.IOException;
import org.json.JSONObject;
/** Process-scoped original Bionic ownership beneath the ordinary Java entry. */
final class BionicNativeRuntime {
 private static boolean initialized;
 private static native long prepare0(String[] options);
 private static native long close0();
 static boolean invalidIdentity(String value){return value.chars().anyMatch(c->c==0||c=='\r'||c=='\n'||c=='\t');}
 static void prepare(LaunchConfig config,ProcessAuthority authority)throws Exception{
  Path runtime=config.directory("runtimeRoot"),descriptor=runtime.resolve("native/bionic/profile.json");
  if(!Files.isRegularFile(descriptor)||!"arm64-v8a".equals(System.getenv("MACLAUNCHER_ANDROID_NATIVE_ABI")))return;
  if(initialized)throw new IllegalStateException("Android native runtime is already initialized");
  byte[] bytes;try(var input=Files.newInputStream(config.runtimeFile("native/bionic/profile.json"))){bytes=input.readNBytes(16385);}if(bytes.length>16384)throw new IllegalArgumentException("Android native profile is too large");
  JSONObject profile=new JSONObject(new String(bytes,StandardCharsets.UTF_8));
  if(profile.getInt("schemaVersion")!=1||!profile.getString("nativeAbi").equals("arm64-v8a"))throw new IllegalArgumentException("Unsupported Android native profile");
  if(authority==null)throw new SecurityException("Android native execution requires its supervised process identity");
  Path state=config.directory("cacheRoot").resolve("bionic"),map=state.resolve("namespace-path-map").toRealPath();
  if(!map.startsWith(config.directory("cacheRoot"))||!Files.isRegularFile(map))throw new SecurityException("Android native namespace is outside its private cache");
  Path resolved=state.resolve("bootstrap.json").toRealPath();
  if(!resolved.startsWith(config.directory("cacheRoot"))||Files.size(resolved)>16384)throw new SecurityException("Android native bootstrap is outside its private cache");
  JSONObject local=new JSONObject(Files.readString(resolved));
  if(local.getInt("schemaVersion")!=1||!local.getString("imageSha256").equals(profile.getJSONObject("systemImage").getJSONObject("acquisition").getString("sha256")))throw new SecurityException("Android native system image differs from its runtime");
  Path root=Path.of(local.getString("systemRoot")).toRealPath();
  String bootstrapName=profile.getString("inodeBootstrap");
  if(!bootstrapName.startsWith("native/bionic/")||Arrays.stream(bootstrapName.split("/",-1)).anyMatch(part->part.isEmpty()||part.equals(".")||part.equals("..")))throw new SecurityException("Android native storage helper path is invalid");
  Path bootstrap=config.runtimeFile(bootstrapName);
  ProcessBuilder metadata=new ProcessBuilder(bootstrap.toString(),"initialize",Integer.toString(authority.self.uid()),config.directory("filesRoot").getParent().toString());
  // This original metadata helper has no JVM or graphics entry. The parent VM
  // retains its complete ordinary preload and sandbox environment.
  metadata.environment().keySet().removeIf(key->key.startsWith("DYLD_"));
  metadata.redirectError(ProcessBuilder.Redirect.INHERIT);metadata.redirectOutput(ProcessBuilder.Redirect.DISCARD);
  if(metadata.start().waitFor()!=0)throw new IllegalStateException("Android native storage metadata could not be prepared");
  if(invalidIdentity(authority.self.processName())||invalidIdentity(authority.self.securityContext()))throw new SecurityException("Android native identity contains invalid control characters");
  String identity="service\t"+authority.self.processName()+"\nuid\t"+authority.self.uid()+"\ngid\t"+authority.self.uid()+"\ncap_effective\t0\ncap_permitted\t0\ncap_bounding\t0\nseclabel\t"+authority.self.securityContext()+"\n";
  int alignment=alignmentFlags(config.directory("nativeRoot"),android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE));
  boolean pageCompat=maclauncher.android.framework.HostPackages.hostPageSizeCompatibility(alignment);
  long status=prepare0(new String[]{config.runtimeFile("native/bionic/libaim_linux_abi.dylib").toString(),root.toString(),map.toString(),"/data/local/tmp/maclauncher-bionic/bootstrap.so",state.resolve("kernel").toString(),config.runtimeFile("native/libnativebinding.dylib").toString(),identity,pageCompat?"1":"0","/data/app/"+config.text("packageName")+"/lib/arm64"});
  if(status!=0)throw new IllegalStateException("Android native runtime initialization failed ("+status+")");initialized=true;
 }
 static int alignmentFlags(Path nativeRoot,long pageSize){
  if(pageSize!=16384)return 0;
  int flags=0;
  try(var libraries=Files.newDirectoryStream(nativeRoot,"*.so")){
   for(Path library:libraries){
    if(!Files.isRegularFile(library,LinkOption.NOFOLLOW_LINKS))continue;
    try(var channel=FileChannel.open(library,StandardOpenOption.READ)){
     long size=channel.size();ByteBuffer header=ByteBuffer.allocate(64).order(ByteOrder.LITTLE_ENDIAN);
     readHeader(channel,header,0);header.flip();
     if(header.getInt(0)!=0x464c457f||header.get(4)!=2||header.get(5)!=1||Short.toUnsignedInt(header.getShort(18))!=183)throw new IOException("Invalid ARM64 native header");
     long offset=header.getLong(32);int entry=Short.toUnsignedInt(header.getShort(54)),count=Short.toUnsignedInt(header.getShort(56));
     if(entry!=56||offset<0||offset>size-(long)entry*count)throw new IOException("Invalid ARM64 program headers");
     long minimum=pageSize;ByteBuffer program=ByteBuffer.allocate(entry).order(ByteOrder.LITTLE_ENDIAN);
     for(int i=0;i<count;i++){
      program.clear();readHeader(channel,program,offset+(long)i*entry);program.flip();if(program.getInt(0)!=1)continue;
      long alignment=program.getLong(48);
      // Match original ElfReader::CheckProgramHeaderAlignment: ignore 0/1 and
      // non-power-of-two alignment; original loader keeps all layout checks.
      if(alignment>1&&(alignment&(alignment-1))==0)minimum=Math.min(minimum,alignment);
     }
     if(minimum<pageSize)flags|=android.content.pm.ApplicationInfo.PAGE_SIZE_APP_COMPAT_FLAG_ELF_NOT_ALIGNED;
    }
   }
   return flags;
  }catch(IOException|RuntimeException failure){
   System.err.println("Android native alignment inspection failed: "+failure.getMessage());
   return android.content.pm.ApplicationInfo.PAGE_SIZE_APP_COMPAT_FLAG_ERROR;
  }
 }
 private static void readHeader(FileChannel channel,ByteBuffer buffer,long offset)throws IOException{
  while(buffer.hasRemaining()){
   int read=channel.read(buffer,offset+buffer.position());if(read<0)throw new IOException("Truncated ARM64 native header");
   if(read==0)throw new IOException("ARM64 native header could not be read");
  }
 }
 static void retire(){
  if(!initialized)return;long status=close0();
  // Busy entries and Java threads retain their original VM-lifetime owner.
  // Never terminate the admission service beneath those continuations.
  if(status!=0&&status!=-16)System.err.println("Android native runtime retirement failed ("+status+")");
 }
}
