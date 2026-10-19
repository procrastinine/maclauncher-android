package maclauncher.android.framework;

import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import java.io.IOException;
import java.lang.ref.Cleaner;
import java.util.Objects;
import java.util.concurrent.*;

/** Original Binder RPC sessions over launcher-owned Unix socket pairs. */
public final class HostBinderRpc {
    private HostBinderRpc() {}
    /** The authenticated endpoint disappeared during transport establishment. */
    public static final class PeerUnavailableException extends IOException {
        public PeerUnavailableException(String message){super(message);}
    }
    /** Supplied by the process supervisor for the actual process at the other end. */
    public record Peer(String packageName,String processName,int pid,int uid,String securityContext) {
        public Peer(String packageName,String processName,int pid,int uid){this(packageName,processName,pid,uid,"u:r:untrusted_app:s0");}
        public Peer {Objects.requireNonNull(securityContext);if(securityContext.isBlank()||securityContext.indexOf('\0')>=0)throw new IllegalArgumentException("Invalid Android security context");Objects.requireNonNull(packageName);Objects.requireNonNull(processName);if(packageName.isBlank()||processName.isBlank()||pid<=0||uid<0)throw new IllegalArgumentException("Invalid Android process identity");}
    }
    private static final Cleaner CLEANER=Cleaner.create();
    // A slow callback in one session must not retain every unrelated session's
    // sockets and native threads. State submits exactly one retirement per owner;
    // idle workers disappear instead of retaining a global serial cleanup queue.
    private static final ExecutorService RETIREMENTS=Executors.newCachedThreadPool(operation->{Thread thread=new Thread(operation,"Android Binder cleanup");thread.setDaemon(true);return thread;});
    private static final class State implements Runnable {
        private long pointer;
        final CompletableFuture<Void> retired=new CompletableFuture<>();
        State(long pointer){this.pointer=pointer;}
        synchronized IBinder root(){if(pointer==0)throw new IllegalStateException("Android Binder session is closed");return rootNative(pointer);}
        @Override public void run(){long value;synchronized(this){value=pointer;pointer=0;}if(value==0)return;RETIREMENTS.execute(()->{try{closeNative(value);retired.complete(null);}catch(Throwable failure){retired.completeExceptionally(failure);}});}
    }
    public static final class Connection implements AutoCloseable {
        private final State state;
        private final Cleaner.Cleanable cleaner;
        private final IBinder root;
        private final Peer peer;
        private Connection(long pointer,Peer peer){this.peer=peer;state=new State(pointer);Cleaner.Cleanable candidate=null;try{root=state.root();candidate=CLEANER.register(this,state);cleaner=candidate;}catch(Throwable failure){state.run();throw failure;}}
        public Peer peer(){return peer;}
        public IBinder root(){synchronized(state){if(state.pointer==0)throw new IllegalStateException("Android Binder session is closed");return root;}}
        public CompletionStage<Void> closed(){return state.retired.minimalCompletionStage();}
        @Override public void close(){cleaner.clean();}
    }
    public static final class Listener implements AutoCloseable {
        private final State state;
        private final Cleaner.Cleanable cleaner;
        private Listener(long pointer){state=new State(pointer);try{cleaner=CLEANER.register(this,state);}catch(Throwable failure){state.run();throw failure;}}
        public CompletionStage<Void> closed(){return state.retired.minimalCompletionStage();}
        @Override public void close(){cleaner.clean();}
    }
    /** One profile-owned listener accepts only supervisor-enrolled actual PIDs. */
    public static Listener listen(String path,IBinder root,HostProcessServices processes,int threads)throws IOException{
        Objects.requireNonNull(path);Objects.requireNonNull(root);Objects.requireNonNull(processes);
        if(threads<1)throw new IllegalArgumentException("Binder service needs an incoming thread");
        return new Listener(listenNative(path,root,processes,threads));
    }
    public static Connection connect(String path,Peer peer,int callbackThreads)throws IOException{
        Objects.requireNonNull(path);Objects.requireNonNull(peer);
        if(callbackThreads<0)throw new IllegalArgumentException("Negative Binder callback thread count");
        return new Connection(connectPathNative(path,peer,callbackThreads),peer);
    }
    static Connection accepted(long pointer,Peer peer){return new Connection(pointer,peer);}
    public static Connection connect(ParcelFileDescriptor bootstrap,Peer peer,int callbackThreads)throws IOException{
        Objects.requireNonNull(bootstrap);Objects.requireNonNull(peer);if(callbackThreads<0)throw new IllegalArgumentException("Negative Binder callback thread count");
        return new Connection(connectNative(bootstrap.getFd(),peer,callbackThreads),peer);
    }
    public static Connection serve(ParcelFileDescriptor bootstrap,Peer peer,IBinder root,int incomingThreads)throws IOException{
        Objects.requireNonNull(bootstrap);Objects.requireNonNull(peer);Objects.requireNonNull(root);if(incomingThreads<1)throw new IllegalArgumentException("Binder service needs an incoming thread");
        return new Connection(serveNative(bootstrap.getFd(),peer,root,incomingThreads),peer);
    }
    /** Null outside an authenticated RPC dispatch, including ordinary local calls. */
    public static native Peer callingPeer();
    /** Actual transport owner of a remote Binder; null for a local object. */
    public static native Peer ownerOf(IBinder binder);
    private static native long connectNative(int bootstrap,Peer peer,int threads)throws IOException;
    private static native long serveNative(int bootstrap,Peer peer,IBinder root,int threads)throws IOException;
    private static native long connectPathNative(String path,Peer peer,int threads)throws IOException;
    private static native long listenNative(String path,IBinder root,HostProcessServices processes,int threads)throws IOException;
    private static native IBinder rootNative(long pointer);
    private static native void closeNative(long pointer);
}
