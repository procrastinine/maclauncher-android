package maclauncher.android.framework;

import android.app.*;
import android.content.*;
import android.content.pm.ProviderInfo;
import android.os.*;
import java.util.*;

/** Remote references to ActivityThread-owned providers, including publication races. */
final class HostProviderConnections implements AutoCloseable {
    private final Context context;
    private final ActivityThread thread;
    private final Map<IBinder,Reference> references=new HashMap<>();
    private final Set<String> published=new HashSet<>();
    private boolean closed;

    HostProviderConnections(Context context){this.context=context;thread=Objects.requireNonNull(ActivityThread.currentActivityThread());}

    private final class Reference extends Binder implements IBinder.DeathRecipient {
        final IApplicationThread caller;
        final HostBinderRpc.Peer peer;
        final String authority;
        final ContentProviderHolder holder;
        int stable,unstable;
        boolean waiting,retired;
        Reference(IApplicationThread caller,HostBinderRpc.Peer peer,ProviderInfo info,String authority,boolean stable){
            this.caller=caller;this.peer=peer;this.authority=authority;this.stable=stable?1:0;unstable=stable?0:1;
            holder=new ContentProviderHolder(info);holder.connection=this;
        }
        @Override public void binderDied(){retire(this);}
        void authorize(){if(!Objects.equals(peer,HostBinderRpc.callingPeer()))throw new SecurityException("Provider reference belongs to another process");}
    }

    ContentProviderHolder acquire(IApplicationThread caller,HostBinderRpc.Peer peer,ProviderInfo info,String authority,boolean stable)throws RemoteException{
        Objects.requireNonNull(caller,"Content provider caller");
        // Use the original component-permission rule and signed package catalog.
        // Exact URI/path/read/write and attribution checks remain in ContentProvider.
        boolean permitted=permitted(info.readPermission,info,peer.uid())||permitted(info.writePermission,info,peer.uid());
        if(!permitted&&info.pathPermissions!=null)for(var path:info.pathPermissions){
            if(path.getReadPermission()!=null&&permitted(path.getReadPermission(),info,peer.uid())||path.getWritePermission()!=null&&permitted(path.getWritePermission(),info,peer.uid())){permitted=true;break;}
        }
        if(!permitted)throw new SecurityException("Permission denied for content provider "+authority);
        Reference reference=new Reference(caller,peer,info,authority,stable);
        caller.asBinder().linkToDeath(reference,0);
        synchronized(this){
            if(closed||reference.retired){unlink(reference);return null;}
            // ActivityThread is the sole owner of provider instances and authority maps.
            // Publication and this check share the connection lock, so a provider
            // cannot become available between the lookup and waiter registration.
            reference.holder.provider=published.contains(info.name)?thread.acquireExistingProvider(context,authority,context.getUserId(),stable):null;
            reference.waiting=reference.holder.provider==null;
            references.put(reference,reference);
        }
        return reference.holder;
    }
    private static boolean permitted(String permission,ProviderInfo info,int uid){return ActivityManager.checkComponentPermission(permission,uid,info.applicationInfo.uid,info.exported)==android.content.pm.PackageManager.PERMISSION_GRANTED;}

    void published(List<ContentProviderHolder> providers){
        List<Reference> ready=new ArrayList<>();
        synchronized(this){
            if(closed)return;
            for(var original:providers)published.add(original.info.name);
            for(var original:providers)for(var reference:references.values()){
                if(!reference.waiting||!Objects.equals(reference.holder.info.name,original.info.name))continue;
                reference.holder.provider=original.provider;reference.waiting=false;ready.add(reference);
            }
        }
        // This is Android's original asynchronous publication callback. It wakes
        // acquireProvider directly, without a main-Looper task or a polling thread.
        for(var reference:ready)try{reference.caller.notifyContentProviderPublishStatus(reference.holder,reference.authority,context.getUserId(),true);}
        catch(RemoteException gone){retire(reference);}
    }

    boolean ref(IBinder token,int stable,int unstable){
        Reference reference;boolean retire;
        synchronized(this){
            reference=references.get(token);if(reference==null)return false;reference.authorize();
            if(reference.stable+stable<0||reference.unstable+unstable<0)throw new IllegalStateException("Content provider reference count is negative");
            reference.stable+=stable;reference.unstable+=unstable;retire=reference.stable==0&&reference.unstable==0;
            if(retire){reference.retired=true;references.remove(reference,reference);}
        }
        if(retire)unlink(reference);return true;
    }
    void remove(IBinder token,boolean stable){ref(token,stable?-1:0,stable?0:-1);}
    void died(IBinder token){Reference reference;synchronized(this){reference=references.get(token);if(reference==null)return;reference.authorize();}retire(reference);}
    private void retire(Reference reference){
        synchronized(this){if(reference.retired)return;reference.retired=true;references.remove(reference,reference);}
        unlink(reference);
        // References never close the original provider, cursor or process. Their
        // owners retain those lifetimes, including other clients' active queries.
    }
    private void unlink(Reference reference){
        try{reference.caller.asBinder().unlinkToDeath(reference,0);}catch(NoSuchElementException gone){}
    }
    @Override public void close(){
        List<Reference> active;synchronized(this){if(closed)return;closed=true;active=List.copyOf(references.values());}
        for(var reference:active){
            if(reference.waiting)try{reference.caller.notifyContentProviderPublishStatus(reference.holder,reference.authority,context.getUserId(),false);}catch(RemoteException gone){}
            retire(reference);
        }
    }
}
