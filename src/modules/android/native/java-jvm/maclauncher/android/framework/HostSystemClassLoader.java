package maclauncher.android.framework;

import java.io.IOException;
import java.lang.reflect.*;
import java.net.URL;
import java.util.*;

/** JVM system lookups also reach the current Android application's defining
 * loader. The original host loader still owns platform and instrumentation code. */
public final class HostSystemClassLoader extends ClassLoader {
 static {registerAsParallelCapable();}
 private final ThreadLocal<Set<String>> resolving=ThreadLocal.withInitial(HashSet::new);
 public HostSystemClassLoader(ClassLoader parent){super(Objects.requireNonNull(parent));}
 private ClassLoader application(){ClassLoader value=HostContext.hostClassLoader();return value==this||value==getParent()?null:value;}
 @Override protected Class<?> loadClass(String name,boolean resolve)throws ClassNotFoundException {
  ClassNotFoundException original;
  try{return super.loadClass(name,resolve);}catch(ClassNotFoundException failure){original=failure;}
  ClassLoader application=application();if(application==null)throw original;Set<String> active=resolving.get();
  if(!active.add(name))throw original;
  try{Class<?> value=application.loadClass(name);if(resolve)resolveClass(value);return value;}
  catch(ClassNotFoundException failure){failure.addSuppressed(original);throw failure;}
  finally{active.remove(name);if(active.isEmpty())resolving.remove();}
 }
 @Override public URL getResource(String name){
  URL value=super.getResource(name);if(value!=null)return value;
  ClassLoader application=application();if(application==null)return null;
  Set<String> active=resolving.get();String key="resource:"+name;if(!active.add(key))return null;
  try{return application.getResource(name);}finally{active.remove(key);if(active.isEmpty())resolving.remove();}
 }
 @Override public Enumeration<URL> getResources(String name)throws IOException {
  var resources=new LinkedHashMap<String,URL>();
  for(var values=super.getResources(name);values.hasMoreElements();){URL value=values.nextElement();resources.putIfAbsent(value.toExternalForm(),value);}
  ClassLoader application=application();if(application==null)return Collections.enumeration(resources.values());
  Set<String> active=resolving.get();String key="resources:"+name;
  if(active.add(key))try{for(var values=application.getResources(name);values.hasMoreElements();){URL value=values.nextElement();resources.putIfAbsent(value.toExternalForm(),value);}}
  finally{active.remove(key);if(active.isEmpty())resolving.remove();}
  return Collections.enumeration(resources.values());
 }
 /** The JVM calls this standard custom-system-loader hook for startup agents
  * and Instrumentation.appendToSystemClassLoaderSearch. Append to the original
  * host loader, so agents remain visible to the application's platform parent. */
 public void appendToClassPathForInstrumentation(String path){
  try{
   Method append=getParent().getClass().getDeclaredMethod("appendToClassPathForInstrumentation",String.class);append.setAccessible(true);append.invoke(getParent(),path);
  }catch(InvocationTargetException failure){Throwable cause=failure.getCause();if(cause instanceof RuntimeException runtime)throw runtime;if(cause instanceof Error error)throw error;throw new IllegalStateException("JVM instrumentation path is unavailable",cause);}
  catch(ReflectiveOperationException failure){throw new IllegalStateException("JVM instrumentation path is unavailable",failure);}
 }
}
