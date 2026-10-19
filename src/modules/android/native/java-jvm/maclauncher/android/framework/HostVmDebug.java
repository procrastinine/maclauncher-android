package maclauncher.android.framework;

import java.lang.management.ManagementFactory;

/** Runtime counters from the JVM which owns the original Android classes. */
public final class HostVmDebug {
 private HostVmDebug(){}
 public static int getLoadedClassCount(){return ManagementFactory.getClassLoadingMXBean().getLoadedClassCount();}
 private static long collections(boolean time){
  long total=0;boolean available=false;
  for(var collector:ManagementFactory.getGarbageCollectorMXBeans()){
   long count=time?collector.getCollectionTime():collector.getCollectionCount();
   if(count>=0){total+=count;available=true;}
  }
  return available?total:-1;
 }
 private static long allocated(){
  var threads=ManagementFactory.getThreadMXBean();
  return threads instanceof com.sun.management.ThreadMXBean extended&&extended.isThreadAllocatedMemorySupported()&&extended.isThreadAllocatedMemoryEnabled()?extended.getTotalThreadAllocatedBytes():-1;
 }
 public static String getRuntimeStatInternal(int statistic){
  // The original VMDebug name-to-index table owns the public contract. Counters
  // with no equivalent in this JVM retain an explicit unavailable value.
  return switch(statistic){case 0->Long.toString(collections(false));case 1->Long.toString(collections(true));case 2->Long.toString(allocated());case 3,4,5,8,9,10->"-1";case 6,7->"";default->null;};
 }
 public static String[] getRuntimeStatsInternal(){
  String[] result=new String[11];for(int i=0;i<result.length;i++)result[i]=getRuntimeStatInternal(i);return result;
 }
}
