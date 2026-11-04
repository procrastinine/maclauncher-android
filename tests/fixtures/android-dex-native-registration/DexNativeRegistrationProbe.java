import java.nio.file.*;
import java.lang.reflect.*;
import java.util.*;
import dalvik.system.*;
import maclauncher.android.*;
import maclauncher.android.framework.*;
import maclauncher.android.bridge.GuestNativeLoader;
public final class DexNativeRegistrationProbe {
 static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
 static Set<String> names(DexFile file){return new HashSet<>(Collections.list(file.entries()));}
 public static void main(String[] args)throws Exception {
  Path runtime=Path.of(args[0]),jar=Path.of(args[1]),data=Files.createDirectories(Path.of(args[2]));
  boolean baseline=args[3].equals("-");
  HostFilesystem.configure(runtime,data,data,data,"owned.dex.registration");
  System.setProperty("klepton.graphics",runtime.resolve("native/libklepton-graphics.dylib").toString());
  System.load(runtime.resolve("native/libklepton-jvm.dylib").toString());
  System.load(runtime.resolve("native/libnativebinding.dylib").toString());
  HostProcessIdentity.initializeIds();
  HostFilesystem.installNativeMappings(List.of(),data);
  JvmNativeMemory.configure(runtime.resolve("native/libandroid_runtime.dylib"));JvmNativeMemory.initialize();
  if(!baseline)System.load(args[3]);
  var loader=HostDexFiles.application(jar,List.of(),data,DexNativeRegistrationProbe.class.getClassLoader());
  Field pathField=BaseDexClassLoader.class.getDeclaredField("pathList");pathField.setAccessible(true);
  Object path=pathField.get(loader);Field elementsField=path.getClass().getDeclaredField("dexElements");elementsField.setAccessible(true);
  Object element=((Object[])elementsField.get(path))[0];Field fileField=element.getClass().getDeclaredField("dexFile");fileField.setAccessible(true);
  DexFile dex=(DexFile)fileField.get(element);
  Set<String> original=names(dex);check(original.containsAll(Set.of("owned.Rebinder","fixture.Value")),"Actual prepared DEX enumeration");
  HostContext.hostPackage("owned.dex.registration","1",1);
  HostContext.hostApplicationInfo(false,jar.toString(),data.toString(),data.toString(),28);
  Path nativeRoot=Path.of(args[4]);
  GuestNativeLoader.configure(runtime.resolve("native/libklepton-jvm.dylib").toString(),nativeRoot.toString(),jar.toString(),loader);
  Class<?> rebinder=loader.loadClass("owned.Rebinder");
  if(baseline){
   try{rebinder.getMethod("load").invoke(null);throw new AssertionError("Original missing native declaration did not fail");}
   catch(InvocationTargetException failure){Throwable at=failure;while(at!=null&&!(at instanceof NoSuchMethodError))at=at.getCause();check(at!=null&&at.getMessage().contains("getClassNameList"),"Original RegisterNatives error");}
   System.out.println("PASS original runtime rejects replacement after native declaration was converted to Java");return;
  }
  Method enumeration=DexFile.class.getDeclaredMethod("getClassNameList",Object.class);
  check(Modifier.isNative(enumeration.getModifiers()),"Original native flag");
  check(!DexFile.isReadOnlyJavaDclEnforced(),"Original host DCL policy");
  rebinder.getMethod("load").invoke(null);
  check(names(dex).equals(Set.of("fixture.Rebound")),"Actual guest RegisterNatives replacement");
  check((int)rebinder.getMethod("unregister").invoke(null)==0,"Original UnregisterNatives result");
  check(names(dex).equals(original),"Normal JNI symbol resolution restores original enumeration");
  check((int)loader.loadClass("fixture.Value").getMethod("answer").invoke(null)==7,"Original class definition after unregister");
  dex.close();
  try{names(dex);throw new AssertionError("Closed cookie accepted");}
  catch(IllegalStateException expected){check(expected.getMessage().contains("closed"),"Original closed-cookie failure");}
  System.out.println("PASS original DexFile native registration, replacement, unregister, class definition and closed-cookie errors");
 }
}
