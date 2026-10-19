/* Stateless portability operations adapted from Robolectric AndroidInterceptors.
 * Copyright (c) 2010 Xtreme Labs, Pivotal Labs and Google Inc.
 * MIT License; see notices/robolectric-portability/LICENSE.
 */
package maclauncher.android.framework;

import java.lang.reflect.Array;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;

/** JVM equivalents of libcore operations; no service state or simulated devices. */
public final class JvmPortability extends java.io.Writer {
    // At most 32 KiB of UTF-8 output per diagnostic, including its marker.
    // This bounds owned buffering, not arbitrary guest Throwable method work.
    private static final int LOG_CHAR_LIMIT=8192;
    private final StringBuilder text=new StringBuilder(256);
    private boolean truncated;
    private JvmPortability(){}
    public static Object newUnpaddedArray(Object runtime,Class<?> component,int length){
        Objects.requireNonNull(runtime);return Array.newInstance(component,length);
    }
    public static Map.Entry<?,?> eldest(LinkedHashMap<?,?> map){
        return map.isEmpty()?null:map.entrySet().iterator().next();
    }
    public static String adjustLanguageCode(String language){
        String adjusted=language.toLowerCase(Locale.US);
        if(language.equals("he"))adjusted="iw";
        else if(language.equals("id"))adjusted="in";
        else if(language.equals("yi"))adjusted="ji";
        return adjusted;
    }
    public static void logE(String message){log("System.logE: ",message,null);}
    public static void logW(String message){log("System.logW: ",message,null);}
    public static void logE(String message,Throwable error){log("System.logE: ",message,error);}
    public static void logW(String message,Throwable error){log("System.logW: ",message,error);}
    private static void log(String prefix,String message,Throwable error){
        // Format outside the shared PrintStream monitor: guest Throwable methods
        // must not hold up other threads' logging while they execute.
        var text=new JvmPortability();var output=new java.io.PrintWriter(text);
        output.print(prefix);output.println(message);if(error!=null)error.printStackTrace(output);output.flush();System.err.print(text.result());
    }
    @Override public void write(char[] value,int offset,int length){
        Objects.checkFromIndexSize(offset,length,value.length);
        int count=Math.min(length,LOG_CHAR_LIMIT-text.length());text.append(value,offset,count);truncated|=count<length;
    }
    @Override public void write(String value,int offset,int length){
        Objects.checkFromIndexSize(offset,length,value.length());
        int count=Math.min(length,LOG_CHAR_LIMIT-text.length());text.append(value,offset,offset+count);truncated|=count<length;
    }
    @Override public void flush(){}
    @Override public void close(){}
    private String result(){
        if(truncated){String marker="\n[diagnostic truncated]\n";text.setLength(Math.min(text.length(),LOG_CHAR_LIMIT-marker.length()));text.append(marker);}
        return text.toString();
    }
}
