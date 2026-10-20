package maclauncher.test;

import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;

/** Retain the original exit caller while ordinary VM shutdown hooks run. */
public final class ExitTrace {
    private static final AtomicBoolean installed=new AtomicBoolean();
    private ExitTrace(){}
    public static void install(){
        if(!installed.compareAndSet(false,true))return;
        Runtime.getRuntime().addShutdownHook(new Thread(ExitTrace::record,"Android test exit diagnostics"));
    }
    private static void record(){
        try{
            var stacks=new ArrayList<>(Thread.getAllStackTraces().entrySet());
            stacks.sort(Comparator.comparing(entry->entry.getKey().getName()));
            StringBuilder out=new StringBuilder("[android-test] VM shutdown stack snapshot\n");
            int remaining=128;
            for(var entry:stacks){
                if(remaining--==0){out.append("[android-test] Further threads omitted\n");break;}
                Thread thread=entry.getKey();
                out.append('"').append(thread.getName()).append("\" ").append(thread.getState()).append('\n');
                var frames=entry.getValue();
                for(int index=0;index<Math.min(frames.length,96);index++)out.append("\tat ").append(frames[index]).append('\n');
                if(frames.length>96)out.append("\tFurther frames omitted\n");
            }
            System.err.print(out);
        }catch(Throwable failure){
            // Observation never replaces or prevents the original shutdown.
        }
    }
}
