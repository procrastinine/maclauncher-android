package maclauncher.android.framework;

import android.content.*;
import android.database.IContentObserver;
import android.net.Uri;
import android.os.*;
import com.android.server.content.ContentService;
import java.util.*;

/** Local service endpoint using AOSP's observer tree, matching and dispatcher.
 * Observers and their Handlers remain ordinary framework objects. */
public final class HostContentService extends IContentService.Default implements AutoCloseable {
    private final ContentService.ObserverNode observers=new ContentService.ObserverNode("");
    private final Set<IContentObserver> registered=Collections.newSetFromMap(new IdentityHashMap<>());
    private final Map<Uri,Bundle> cache=new LinkedHashMap<>();
    private final int user;
    private final String packageName;
    private boolean closed,syncReported;
    public HostContentService(Context context){user=context.getUserId();packageName=context.getPackageName();}
    private int ownUser(int supplied){
        if(supplied==UserHandle.USER_CURRENT||supplied==UserHandle.USER_ALL)return user;
        if(supplied!=user)throw new SecurityException("Another Android user's content is unavailable");
        return supplied;
    }
    private void requireOpen(){if(closed)throw new IllegalStateException("Content service has closed");}
    @Override public void registerContentObserver(Uri uri,boolean descendants,IContentObserver observer,int suppliedUser,int sdk){
        Objects.requireNonNull(uri);Objects.requireNonNull(observer);int userId=ownUser(suppliedUser);
        synchronized(observers){requireOpen();observers.addObserverLocked(uri,observer,descendants,observers,android.os.Process.myUid(),android.os.Process.myPid(),userId);registered.add(observer);}
    }
    @Override public void unregisterContentObserver(IContentObserver observer){synchronized(observers){observers.removeObserverLocked(Objects.requireNonNull(observer));registered.remove(observer);}}
    @Override public void notifyChange(Uri[] uris,IContentObserver source,boolean wantsSelf,int flags,int suppliedUser,int sdk,String caller){
        int userId=ownUser(suppliedUser);var collector=new ContentService.ObserverCollector();
        synchronized(observers){requireOpen();for(Uri uri:uris){observers.collectObserversLocked(Objects.requireNonNull(uri),0,source,wantsSelf,flags,userId,collector);}cache.clear();}
        collector.dispatch();
    }
    @Override public void putCache(String caller,Uri key,Bundle value,int suppliedUser){
        ownUser(suppliedUser);if(!Objects.equals(packageName,caller))throw new SecurityException("Another application's content cache is unavailable");
        synchronized(observers){requireOpen();if(value==null)cache.remove(key);else{if(cache.size()>=128&&!cache.containsKey(key))cache.remove(cache.keySet().iterator().next());cache.put(key,new Bundle(value));}}
    }
    @Override public Bundle getCache(String caller,Uri key,int suppliedUser){
        ownUser(suppliedUser);if(!Objects.equals(packageName,caller))throw new SecurityException("Another application's content cache is unavailable");
        synchronized(observers){requireOpen();var value=cache.get(key);return value==null?null:new Bundle(value);}
    }
    private synchronized void missingSync(){if(!syncReported){syncReported=true;System.err.println("[android-host] Account synchronization service could not be found");}}
    @Override public void requestSync(android.accounts.Account account,String authority,Bundle extras,String caller){missingSync();}
    @Override public void sync(SyncRequest request,String caller){missingSync();}
    @Override public void syncAsUser(SyncRequest request,int userId,String caller){ownUser(userId);missingSync();}
    @Override public java.util.List<PeriodicSync> getPeriodicSyncs(android.accounts.Account account,String provider,ComponentName component){return List.of();}
    @Override public java.util.List<SyncInfo> getCurrentSyncs(){return List.of();}
    @Override public java.util.List<SyncInfo> getCurrentSyncsAsUser(int userId){ownUser(userId);return List.of();}
    @Override public SyncAdapterType[] getSyncAdapterTypes(){return new SyncAdapterType[0];}
    @Override public SyncAdapterType[] getSyncAdapterTypesAsUser(int userId){ownUser(userId);return getSyncAdapterTypes();}
    @Override public String[] getSyncAdapterPackagesForAuthorityAsUser(String authority,int userId){ownUser(userId);return new String[0];}
    @Override public void close(){synchronized(observers){if(closed)return;closed=true;for(var observer:registered)observers.removeObserverLocked(observer);registered.clear();cache.clear();}}
}
