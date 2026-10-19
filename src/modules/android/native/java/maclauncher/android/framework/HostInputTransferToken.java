package maclauncher.android.framework;
import android.os.*;
import java.util.Objects;
/** The original token owns a retained local Binder capability through the shared native lifetime. */
public final class HostInputTransferToken {
    private HostInputTransferToken(){}
    private static final class State implements AutoCloseable {IBinder token;State(IBinder token){this.token=Objects.requireNonNull(token);}public void close(){token=null;}}
    private static State state(long value){return (State)HostNativeHandle.state(value);}
    public static long nativeCreate(){return nativeCreate(new Binder());}
    public static long nativeCreate(IBinder token){return HostNativeHandle.create(new State(token));}
    public static void nativeWriteToParcel(long value,Parcel parcel){Objects.requireNonNull(parcel).writeStrongBinder(state(value).token);}
    public static long nativeReadFromParcel(Parcel parcel){return nativeCreate(Objects.requireNonNull(parcel).readStrongBinder());}
    public static IBinder nativeGetBinderToken(long value){return state(value).token;}
    public static long nativeGetBinderTokenRef(long value){return Integer.toUnsignedLong(System.identityHashCode(state(value).token));}
    public static long nativeGetNativeInputTransferTokenFinalizer(){return HostNativeHandle.finalizer();}
    public static boolean nativeEquals(long a,long b){return state(a).token==state(b).token;}
}
