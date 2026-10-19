package maclauncher.android.framework;

import android.content.pm.IPackageManager;
import android.os.Process;
import android.os.UserHandle;
import android.os.Binder;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.SecureRandom;
import java.util.HexFormat;
import java.util.ArrayDeque;

/** Installation-owned random identities; never a Mac hardware identifier. */
public final class HostAndroidIdentity {
    private HostAndroidIdentity(){}
    private static Path data;
    private record Caller(long token,HostBinderRpc.Peer peer){}
    private static final ThreadLocal<ArrayDeque<Caller>> callers=ThreadLocal.withInitial(ArrayDeque::new);
    /** Retain only the authenticated installation identity across the original
     * provider's temporary Binder identity clear; its caller checks run first. */
    public static long clearCallingIdentity(){var peer=HostBinderRpc.callingPeer();long token=Binder.clearCallingIdentity();callers.get().push(new Caller(token,peer));return token;}
    public static void restoreCallingIdentity(long token){Binder.restoreCallingIdentity(token);var stack=callers.get();if(!stack.isEmpty()&&stack.peek().token==token)stack.pop();if(stack.isEmpty())callers.remove();}
    public static void prepare(Path root)throws IOException {data=root.toRealPath();}
    /** Runs in the existing application sandbox, before the authenticated
     * identity packet. The profile owner never maps another app's private data. */
    public static String localId()throws IOException {
        Path root=data;if(root==null)return null;
        Path file=root.resolve("system/android-id");Files.createDirectories(file.getParent());
        try(var channel=FileChannel.open(file,StandardOpenOption.CREATE,StandardOpenOption.READ,StandardOpenOption.WRITE,LinkOption.NOFOLLOW_LINKS);var lock=channel.lock()){
            ByteBuffer bytes=ByteBuffer.allocate(16);while(bytes.hasRemaining()&&channel.read(bytes)>0){}
            String existing=new String(bytes.array(),0,bytes.position(),StandardCharsets.US_ASCII);
            if(existing.matches("[0-9a-f]{16}"))return existing;
            byte[] random=new byte[8];new SecureRandom().nextBytes(random);String id=HexFormat.of().formatHex(random);
            channel.position(0);channel.truncate(0);bytes=ByteBuffer.wrap(id.getBytes(StandardCharsets.US_ASCII));while(bytes.hasRemaining())channel.write(bytes);channel.force(false);return id;
        }
    }
    /** The original provider retains its SSAID persistence and caller checks.
     * Its external identity source preserves the existing installation value. */
    public static String applicationId(IPackageManager packages,String packageName,int user)throws android.os.RemoteException {
        if(Process.myUid()!=Process.SYSTEM_UID)throw new SecurityException("Android identity storage belongs to the system process");
        var stack=callers.get();var peer=stack.isEmpty()?HostBinderRpc.callingPeer():stack.peek().peer;
        String value=user==UserHandle.myUserId()?HostPackages.androidId(peer,packageName):null;
        return value==null?packages.getInstantAppAndroidId(packageName,user):value;
    }
}
