package maclauncher.android.framework;

import android.util.EventLog;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.charset.CodingErrorAction;
import java.util.*;

/** Process-local binary log storage below the unchanged Android EventLog native API. */
public final class HostEventLog {
    private static final int MAX_EVENT=4096,MAX_BYTES=1<<20,MAX_EVENTS=2048,HEADER=28;
    private record Record(int tag,long time,byte[] bytes){}
    private static final ArrayDeque<Record> events=new ArrayDeque<>();
    private static final java.util.concurrent.atomic.AtomicLong sequence=new java.util.concurrent.atomic.AtomicLong();
    private static int bytes;
    private HostEventLog(){}
    private static void string(ByteBuffer out,String value){
        int start=out.position();out.put((byte)2).putInt(0);
        // Encode directly into the bounded record, preserving UTF-8 boundaries
        // without allocating a second buffer proportional to application input.
        StandardCharsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPLACE)
                .onUnmappableCharacter(CodingErrorAction.REPLACE).encode(CharBuffer.wrap(value),out,true);
        out.putInt(start+1,out.position()-start-5);
    }
    private static void encode(ByteBuffer out,Object value,int depth){
        if(value instanceof Integer number){out.put((byte)0).putInt(number);return;}
        if(value instanceof Long number){out.put((byte)1).putLong(number);return;}
        if(value instanceof Float number){out.put((byte)4).putFloat(number);return;}
        if(value instanceof Object[] values&&depth<8){
            out.put((byte)3);int countAt=out.position();out.put((byte)0);int count=0;
            for(Object child:values){if(count==255||out.remaining()<9)break;encode(out,child,depth+1);count++;}out.put(countAt,(byte)count);return;
        }
        string(out,value==null?"NULL":String.valueOf(value));
    }
    public static int write(int tag,Object value){
        ByteBuffer data=ByteBuffer.allocate(MAX_EVENT).order(ByteOrder.nativeOrder());long now=System.currentTimeMillis()*1_000_000L;
        data.position(HEADER);data.putInt(tag);encode(data,value,0);int length=data.position();
        data.putShort(0,(short)(length-HEADER));data.putShort(2,(short)HEADER);data.putInt(4,android.os.Process.myPid());data.putInt(8,android.os.Process.myTid());data.putInt(12,(int)(now/1_000_000_000L));data.putInt(16,(int)(now%1_000_000_000L));data.putInt(20,2);data.putInt(24,android.os.Process.myUid());
        byte[] encoded=Arrays.copyOf(data.array(),length);
        synchronized(events){while(!events.isEmpty()&&(events.size()>=MAX_EVENTS||bytes+length>MAX_BYTES))bytes-=events.removeFirst().bytes.length;events.addLast(new Record(tag,now,encoded));bytes+=length;}
        return length-HEADER;
    }
    public static int write(int tag,int value){return write(tag,Integer.valueOf(value));}
    public static int write(int tag,long value){return write(tag,Long.valueOf(value));}
    public static int write(int tag,float value){return write(tag,Float.valueOf(value));}
    private static Object[] attributes(List<String> values){return values==null?new Object[0]:values.stream().limit(64).toArray();}
    public static long begin(String name,List<String> values){long id=sequence.incrementAndGet();write(0x4d4c4453,new Object[]{"begin",id,name,attributes(values)});return id;}
    public static void end(long id,List<String> values){write(0x4d4c4453,new Object[]{"end",id,attributes(values)});}
    public static void instant(String name,List<String> values){write(0x4d4c4453,new Object[]{"instant",name,attributes(values)});}
    public static void read(int[] tags,long since,Collection<EventLog.Event> destination){
        Objects.requireNonNull(tags);Objects.requireNonNull(destination);var accepted=new HashSet<Integer>();for(int tag:tags)accepted.add(tag);
        List<Record> snapshot;synchronized(events){snapshot=events.stream().filter(event->accepted.contains(event.tag)&&event.time>=since).toList();}
        for(Record event:snapshot)destination.add((EventLog.Event)HostUiAccess.construct(EventLog.Event.class,new Class<?>[]{byte[].class},(Object)event.bytes.clone()));
    }
}
