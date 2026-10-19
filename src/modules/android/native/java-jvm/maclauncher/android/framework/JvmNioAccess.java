package maclauncher.android.framework;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.invoke.VarHandle;
import java.lang.ref.ReferenceQueue;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.nio.*;
import java.util.*;

/** Android's shared mutable MemoryRef liveness, enforced by the JDK's actual
 * scoped NIO memory accesses. Buffer objects and native cleanup owners stay
 * unchanged; only their optional scope is attached before publishing views. */
public final class JvmNioAccess {
    private static final Field SEGMENT,ADDRESS;
    private static final MethodHandle CREATE_SESSION,CLOSE_SESSION,SEGMENT_SESSION,NEW_SEGMENT;
    private static final VarHandle STATE;
    private static final int OPEN,CLOSED;
    private static final ReferenceQueue<Buffer> DEAD=new ReferenceQueue<>();
    private static final Map<Key,Lease> LEASES=new HashMap<>();
    private static final ClassValue<Field[]> ATTACHMENTS=new ClassValue<>() {
        @Override protected Field[] computeValue(Class<?> type){
            var fields=new ArrayList<Field>();
            for(Class<?> cursor=type;cursor!=null&&Buffer.class.isAssignableFrom(cursor);cursor=cursor.getSuperclass())
                for(String name:new String[]{"att","bb"})try{
                    Field field=cursor.getDeclaredField(name);field.setAccessible(true);fields.add(field);
                }catch(NoSuchFieldException ignored){}
            return fields.toArray(Field[]::new);
        }
    };
    static {
        try {
            Class<?> session=Class.forName("jdk.internal.foreign.MemorySessionImpl");
            Class<?> segment=Class.forName("jdk.internal.foreign.AbstractMemorySegmentImpl");
            Class<?> nativeSegment=Class.forName("jdk.internal.foreign.NativeMemorySegmentImpl");
            var lookup=MethodHandles.privateLookupIn(session,MethodHandles.lookup());
            CREATE_SESSION=lookup.findStatic(session,"createShared",MethodType.methodType(session));
            CLOSE_SESSION=lookup.findVirtual(session,"justClose",MethodType.methodType(void.class));
            STATE=lookup.findVarHandle(session,"state",int.class);
            Field open=session.getDeclaredField("OPEN"),closed=session.getDeclaredField("CLOSED");
            open.setAccessible(true);closed.setAccessible(true);OPEN=open.getInt(null);CLOSED=closed.getInt(null);
            if(OPEN!=0||CLOSED>=0)throw new IllegalStateException("JVM memory-session states are incompatible");
            SEGMENT=Buffer.class.getDeclaredField("segment");SEGMENT.setAccessible(true);
            ADDRESS=Buffer.class.getDeclaredField("address");ADDRESS.setAccessible(true);
            SEGMENT_SESSION=MethodHandles.privateLookupIn(segment,MethodHandles.lookup()).findVirtual(segment,"sessionImpl",MethodType.methodType(session));
            NEW_SEGMENT=MethodHandles.privateLookupIn(nativeSegment,MethodHandles.lookup()).findConstructor(nativeSegment,
                    MethodType.methodType(void.class,long.class,long.class,boolean.class,session));
        }catch(ReflectiveOperationException failure){throw new ExceptionInInitializerError(failure);}
    }
    private JvmNioAccess(){}
    private static final class Key extends WeakReference<Buffer> {
        private final int hash;
        Key(Buffer value,boolean retained){super(value,retained?DEAD:null);hash=System.identityHashCode(value);}
        @Override public int hashCode(){return hash;}
        @Override public boolean equals(Object other){return this==other||other instanceof Key key&&get()!=null&&get()==key.get();}
    }
    private static final class Lease {
        final Object session;
        Lease(){session=invoke(CREATE_SESSION);}
        synchronized void accessible(boolean value){
            int state=(int)STATE.getVolatile(session);
            if(value){
                if(state==CLOSED&&!STATE.compareAndSet(session,CLOSED,OPEN))throw new IllegalStateException("JVM buffer lease changed while reopening");
            }else if(state>=OPEN){
                /* justClose executes the JDK's ScopedMemoryAccess close barrier.
                 * Its resource list is empty: revoking a codec lease must not
                 * release the memory owned by the original native buffer. */
                invoke(CLOSE_SESSION,session);
            }
        }
        boolean accessible(){return (int)STATE.getVolatile(session)>=OPEN;}
    }
    private static Object invoke(MethodHandle method,Object...arguments){
        try{return method.invokeWithArguments(arguments);}
        catch(RuntimeException|Error failure){throw failure;}
        catch(Throwable failure){throw new IllegalStateException("JVM direct-buffer scope failed",failure);}
    }
    private static Object field(Field field,Object owner){
        try{return field.get(owner);}catch(IllegalAccessException failure){throw new IllegalStateException(failure);}
    }
    private static long bytes(Buffer buffer){
        int shift=buffer instanceof ByteBuffer?0:buffer instanceof CharBuffer||buffer instanceof ShortBuffer?1:
                buffer instanceof IntBuffer||buffer instanceof FloatBuffer?2:3;
        return ((long)buffer.capacity())<<shift;
    }
    private static List<Buffer> ancestry(Buffer buffer){
        var result=new ArrayList<Buffer>();var visited=Collections.newSetFromMap(new IdentityHashMap<Buffer,Boolean>());
        Buffer current=buffer;
        while(current!=null&&visited.add(current)){
            result.add(current);Buffer parent=null;
            for(Field attachment:ATTACHMENTS.get(current.getClass()))if(field(attachment,current) instanceof Buffer value&&value.isDirect()){parent=value;break;}
            current=parent;
        }
        return result;
    }
    private static Lease lease(Buffer buffer){
        Objects.requireNonNull(buffer);
        if(!buffer.isDirect())return null;
        synchronized(LEASES){
            for(Object key;(key=DEAD.poll())!=null;)LEASES.remove(key);
            List<Buffer> chain=ancestry(buffer);Buffer root=chain.getLast();
            Lease lease=LEASES.get(new Key(root,false));
            if(lease==null){
                /* An existing foreign-memory owner can be confined or closed
                 * independently. Preserve that scope rather than replace its
                 * safety rules with a codec's reopenable lease. */
                for(Buffer value:chain)if(field(SEGMENT,value)!=null)return null;
                lease=new Lease();LEASES.put(new Key(root,true),lease);
            }
            for(Buffer value:chain){
                Object segment=field(SEGMENT,value);
                if(segment!=null){if(invoke(SEGMENT_SESSION,segment)!=lease.session)throw new IllegalStateException("Direct-buffer views have different memory owners");continue;}
                try {
                    Object owned=invoke(NEW_SEGMENT,ADDRESS.getLong(value),bytes(value),value.isReadOnly(),lease.session);
                    SEGMENT.set(value,owned);
                }catch(IllegalAccessException failure){throw new IllegalStateException(failure);}
            }
            return lease;
        }
    }
    public static void track(Buffer buffer){if(buffer!=null&&buffer.isDirect())lease(buffer);}
    public static void setAccessible(ByteBuffer buffer,boolean value){
        Lease lease=lease(buffer);if(lease==null)throw new UnsupportedOperationException("This buffer has an independent memory owner");
        lease.accessible(value);
    }
    public static boolean isAccessible(ByteBuffer buffer){
        Objects.requireNonNull(buffer);if(!buffer.isDirect())return true;
        Lease lease=lease(buffer);if(lease!=null)return lease.accessible();
        Object segment=field(SEGMENT,buffer);
        return segment==null||(int)STATE.getVolatile(invoke(SEGMENT_SESSION,segment))>=OPEN;
    }
}
