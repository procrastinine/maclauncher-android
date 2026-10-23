package maclauncher.android.bridge;

import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.LongConsumer;

/** Java orchestration only: real ELF metadata, explicit test Method injection. */
public final class PreparedLoadProbe {
    enum Mode { NORMAL, OMIT, DUPLICATE, ZERO, MISMATCH, NATIVE_FAILURE, BIND_FAILURE }
    static final List<String> events=new ArrayList<>();
    static final List<Long> rejected=new ArrayList<>();
    static final AssertionError nativeFailure=new AssertionError("constructor failure");
    static final AssertionError secondaryFailure=new AssertionError("secondary native failure");
    static final AssertionError cleanupFailure=new AssertionError("reject failure");
    static final Method LOAD;
    static final Field STATE;
    static Object state,owner;
    static Mode mode;
    static Throwable callbackFailure;
    static boolean rejectFails;
    static int calls,constructors,controls;
    static {
        try {
            STATE=GuestNativeLoader.class.getDeclaredField("state");STATE.setAccessible(true);
            Class<?> ownerType=Class.forName(GuestNativeLoader.class.getName()+"$Owner");
            LOAD=GuestNativeLoader.class.getDeclaredMethod("load",ownerType,Path.class,boolean.class);LOAD.setAccessible(true);
        } catch(ReflectiveOperationException failure) {throw new ExceptionInInitializerError(failure);}
    }
    static Object field(Object value,String name)throws ReflectiveOperationException {
        Field field=value.getClass().getDeclaredField(name);field.setAccessible(true);return field.get(value);
    }
    @SuppressWarnings("unchecked") static Map<Path,Object> images()throws ReflectiveOperationException {return (Map<Path,Object>)field(state,"loaded");}
    static void reset(Path root,Mode next,List<String> classes)throws Exception {
        events.clear();rejected.clear();calls=constructors=0;callbackFailure=null;rejectFails=false;mode=next;
        Class<?> type=STATE.getType();Constructor<?> ctor=type.getDeclaredConstructor(String.class,Path.class,ClassLoader.class,List.class);ctor.setAccessible(true);
        state=ctor.newInstance("/test-only-runtime",root,new ClassLoader(PreparedLoadProbe.class.getClassLoader()){},classes);
        owner=field(state,"application");
        @SuppressWarnings("unchecked") var cached=(java.util.concurrent.atomic.AtomicReference<Method>)field(owner,"nativeEntry");
        cached.set(Entry.class.getMethod("load",String.class,String.class,String.class,boolean.class));STATE.set(null,state);
    }
    static void load(Path file,boolean initialize)throws Throwable {
        try {LOAD.invoke(null,owner,file,initialize);}catch(InvocationTargetException failure){throw failure.getCause();}
    }
    static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    static void prepared(Path file,long handle)throws Throwable {
        check(!Thread.holdsLock(NativeBinding.class),"NativeBinding monitor spans callback/constructor");
        Object image=images().get(file);
        check(((Long)field(image,"handle"))==handle,"image handle was not published");
        check(field(image,"exports").equals(ElfNativeExports.readMetadata(file).exports()),"original exports not published");
        check(((CompletableFuture<?>)field(image,"linked")).isDone(),"linked future not published before constructors");
        check(!((CompletableFuture<?>)field(image,"constructed")).isDone(),"constructed completed too early");
        check(((Set<?>)field(image,"users")).contains(owner),"owner was not retained");
        int before=calls;load(file,false);check(calls==before,"same-thread constructor re-entry loaded twice");
        CountDownLatch entered=new CountDownLatch(1);
        Thread worker=new Thread(()->{synchronized(NativeBinding.class){entered.countDown();}},"constructor-monitor-control");
        worker.start();check(entered.await(2,TimeUnit.SECONDS),"constructor worker blocked on NativeBinding");worker.join(2000);
        check(!worker.isAlive(),"constructor worker did not retire");
    }
    public static final class Entry {
        public static long load(String runtime,String file,String soname,boolean initialize){throw new AssertionError("ordinary flow used legacy load");}
        public static long loadPrepared(String runtime,String filename,String soname,LongConsumer preparation)throws Throwable {
            check(!Thread.holdsLock(NativeBinding.class),"class monitor held at native entry");
            Path file=Path.of(filename);long handle=1000+ ++calls;events.add("prepare:"+soname);
            if(mode==Mode.OMIT)return handle;
            try {preparation.accept(mode==Mode.ZERO?0:handle);}
            catch(Throwable failure){callbackFailure=failure;if(mode==Mode.BIND_FAILURE)throw secondaryFailure;throw failure;}
            if(mode==Mode.DUPLICATE)preparation.accept(handle);
            prepared(file,handle);constructors++;events.add("construct:"+soname);
            if(mode==Mode.NATIVE_FAILURE)throw nativeFailure;
            return mode==Mode.MISMATCH?handle+1:handle;
        }
        public static void reject(long handle){rejected.add(handle);if(rejectFails)throw cleanupFailure;}
        public static void construct(long handle){throw new AssertionError("ordinary flow used separate construct");}
        public static void initialize(long handle){events.add("initialize:"+handle);}
    }
    static void failure(Path root,Path file,Mode mode,Class<?> expectedCause,boolean publishes)throws Throwable {
        reset(root,mode,mode==Mode.BIND_FAILURE?List.of("fixture.Missing"):List.of("fixture.Unrelated"));
        rejectFails=mode==Mode.NATIVE_FAILURE;
        Throwable first;
        try{load(file,false);throw new AssertionError("expected load failure "+mode);}catch(UnsatisfiedLinkError expected){first=expected;}
        Object image=images().get(file);
        check(((CompletableFuture<?>)field(image,"constructed")).isCompletedExceptionally(),"constructor failure future missing");
        check(field(image,"linkingThread")==null,"linking owner not retired");
        check(expectedCause.isInstance(first.getCause()),"first cause changed: "+first);
        check(rejected.size()==(publishes?1:0),"reject count changed "+mode);
        if(mode==Mode.NATIVE_FAILURE){check(first.getCause()==nativeFailure,"native Throwable replaced");check(Arrays.asList(nativeFailure.getSuppressed()).contains(cleanupFailure),"cleanup failure lost");}
        if(mode==Mode.BIND_FAILURE){
            check(first.getCause()==callbackFailure.getCause(),"first bind Throwable replaced");
            check(Arrays.asList(first.getCause().getSuppressed()).contains(secondaryFailure),"secondary native failure lost");
            check(constructors==0,"constructor ran after failed binding");
        }
        try{load(file,false);throw new AssertionError("cached failed image accepted");}catch(UnsatisfiedLinkError repeated){check(repeated==first,"cached first failure was replaced");}
        controls++;
    }
    public static void main(String[] args)throws Throwable {
        Path root=Path.of(args[0]).toRealPath(),single=root.resolve("libsingle.so"),main=root.resolve("libmain.so");
        check(Modifier.isSynchronized(NativeBinding.class.getDeclaredMethod("loadIn",Method.class,String.class,String.class,String.class,boolean.class).getModifiers()),"legacy load API changed");
        check(!Modifier.isSynchronized(NativeBinding.class.getDeclaredMethod("loadPreparedIn",Method.class,String.class,String.class,String.class,LongConsumer.class).getModifiers()),"prepared entry is synchronized");
        Method api=GuestNativeEntry.class.getDeclaredMethod("loadPrepared",String.class,String.class,String.class,LongConsumer.class);
        check(api.getReturnType()==long.class&&Modifier.isNative(api.getModifiers()),"native prepared descriptor changed");
        for(String old:List.of("construct","initialize","reject"))check(Modifier.isNative(GuestNativeEntry.class.getDeclaredMethod(old,long.class).getModifiers()),"legacy API lost");
        reset(root,Mode.NORMAL,List.of("fixture.Unrelated"));load(main,true);
        check(events.size()==5&&events.get(0).equals("prepare:libdep.so")&&events.get(1).equals("construct:libdep.so")&&events.get(2).equals("prepare:libmain.so")&&events.get(3).equals("construct:libmain.so")&&events.get(4).startsWith("initialize:"),"dependency/constructor/JNI initialization order: "+events);
        check(calls==2&&constructors==2&&rejected.isEmpty(),"normal image ownership changed");
        check(((CompletableFuture<?>)field(images().get(main),"constructed")).isDone(),"normal construction not completed");controls++;
        failure(root,single,Mode.OMIT,UnsatisfiedLinkError.class,false);
        failure(root,single,Mode.ZERO,UnsatisfiedLinkError.class,false);
        failure(root,single,Mode.DUPLICATE,UnsatisfiedLinkError.class,true);
        failure(root,single,Mode.MISMATCH,UnsatisfiedLinkError.class,true);
        failure(root,single,Mode.NATIVE_FAILURE,AssertionError.class,true);
        failure(root,single,Mode.BIND_FAILURE,ClassNotFoundException.class,true);
        reset(root,Mode.NORMAL,List.of("fixture.Unrelated"));
        try{load(root.resolve("libcyclea.so"),false);throw new AssertionError("dependency cycle accepted");}
        catch(UnsatisfiedLinkError expected){check(expected.toString().contains("Recursive Android native dependency"),"recursive dependency error changed");check(calls==0,"cycle reached constructors");}controls++;
        STATE.set(null,null);
        System.out.println("PASS prepared Java orchestration "+controls+" controls; real ELF metadata, Method injection only; no provider execution");
    }
}
