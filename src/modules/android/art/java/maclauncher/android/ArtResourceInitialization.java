package maclauncher.android;

import android.content.res.AssetManager;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructCapUserData;
import android.system.StructCapUserHeader;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

/** Original system assets initialize before application credentials and code. */
final class ArtResourceInitialization {
 private ArtResourceInitialization(){}
 private static final int PR_SET_KEEPCAPS=8,PR_CAPBSET_READ=23,PR_CAPBSET_DROP=24;
 private static final int PR_CAP_AMBIENT=47,PR_CAP_AMBIENT_CLEAR_ALL=4;
 private static final int STARTUP_CAPABILITIES=(1<<1)|(1<<6)|(1<<7)|(1<<8);
 private static final StructCapUserHeader CAP_HEADER=new StructCapUserHeader(OsConstants._LINUX_CAPABILITY_VERSION_3,0);

 static void prepare(int applicationUid)throws Exception{
  if(applicationUid<10000)throw new SecurityException("An installed Android application identity is required");
  if(Os.getuid()!=0||Os.geteuid()!=0||Os.getgid()!=0||Os.getegid()!=0)throw new SecurityException("Android resource initialization requires its startup identity");
  verifyIds(0,STARTUP_CAPABILITIES);
  // Only original immutable platform resources enter this stage. Application
  // APKs, provider loaders and platform-service enrollment follow the drop.
  Class.forName("libcore.util.NativeAllocationRegistry");
  AssetManager.createSystemAssetsInZygoteLocked(false,"/system/framework/framework-res.apk");
  Os.prctl(PR_SET_KEEPCAPS,0,0,0,0);
  Os.prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_CLEAR_ALL,0,0,0);
  for(int capability=0;capability<=OsConstants.CAP_LAST_CAP;capability++)Os.prctl(PR_CAPBSET_DROP,capability,0,0,0);
  // These privileged original Linux calls replace real, effective, saved and
  // filesystem IDs together. The configured supplementary group set is empty.
  Os.setgid(applicationUid);
  Os.setuid(applicationUid);
  Os.capset(CAP_HEADER,new StructCapUserData[]{new StructCapUserData(0,0,0),new StructCapUserData(0,0,0)});
  verifyIds(applicationUid,0);
 }
 private static void verifyIds(int uid,int capabilities)throws Exception{
  if(Os.getuid()!=uid||Os.geteuid()!=uid||Os.getgid()!=uid||Os.getegid()!=uid)throw new SecurityException("Android process credentials differ from their configured identity");
  StructCapUserData[] current=Os.capget(CAP_HEADER);
  if(current.length!=2||current[0].effective!=capabilities||current[0].permitted!=capabilities||current[0].inheritable!=0||current[1].effective!=0||current[1].permitted!=0||current[1].inheritable!=0)throw new SecurityException("Android process capabilities differ from their configured identity");
  String status=new String(Files.readAllBytes(Path.of("/proc/self/status")),java.nio.charset.StandardCharsets.US_ASCII);
  for(String name:new String[]{"Uid:","Gid:"}){
   String[] values=field(status,name).trim().split("\\s+");
   if(values.length!=4)throw new IOException("Android process credential record is incomplete");
   for(String value:values)if(Long.parseLong(value)!=uid)throw new SecurityException("Android saved credentials differ from their configured identity");
  }
  if(Long.parseUnsignedLong(field(status,"CapAmb:").trim(),16)!=0)throw new SecurityException("Android ambient capabilities remain active");
  if(!field(status,"Groups:").trim().isEmpty())throw new SecurityException("Android startup contains unexpected supplementary groups");
  for(int capability=0;capability<=OsConstants.CAP_LAST_CAP;capability++){
   int expected=capability<32&&(capabilities&(1<<capability))!=0?1:0;
   if(Os.prctl(PR_CAPBSET_READ,capability,0,0,0)!=expected)throw new SecurityException("Android capability bounds differ from their configured identity");
  }
 }
 private static String field(String status,String name)throws IOException{
  for(String line:status.split("\\n"))if(line.startsWith(name))return line.substring(name.length());
  throw new IOException("Android process credential record is incomplete");
 }
}
