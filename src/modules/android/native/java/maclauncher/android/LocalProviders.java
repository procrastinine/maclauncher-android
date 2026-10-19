package maclauncher.android;
import android.content.*;
import android.content.pm.ProviderInfo;
import android.app.ActivityThread;
import java.util.*;
/** Installs the process provider set; ActivityThread retains provider ownership. */
public final class LocalProviders implements AutoCloseable {
    private int installedCount;
    private ActivityThread process;
    private Context context;
    private final Thread owner=Thread.currentThread();
    private boolean installed;
    private volatile boolean closed;
    private maclauncher.android.framework.HostContentService contentService;
    public void install(Context context,List<ProviderInfo> inspected){
        requireOwner();if(installed||closed)throw new IllegalStateException("Provider set already initialized");
        installed=true;
        this.context=Objects.requireNonNull(context);process=Objects.requireNonNull(ActivityThread.currentActivityThread(),"Application process is not attached");
        contentService=new maclauncher.android.framework.HostContentService(context);
        maclauncher.android.framework.HostServices.register(ContentResolver.CONTENT_SERVICE_NAME,contentService,"android.content.IContentService");
        List<ProviderInfo> selected=new ArrayList<>();
        for(ProviderInfo original:inspected){
            ProviderInfo info=new ProviderInfo(original);
            if(!info.isEnabled())continue;
            if(info.authority==null)continue;
            selected.add(info);
        }
        selected.sort(Comparator.comparingInt((ProviderInfo info)->info.initOrder).reversed());
        try{
            // Callers supply the manifest's provider set for this process.
            // ActivityThread selects each provider's application context from
            // its ApplicationInfo, including wrapped or overridden app contexts,
            // and owns factory instantiation, attachment and publication.
            try{process.installSystemProviders(selected);}
            finally{Set<ContentProvider> installed=Collections.newSetFromMap(new IdentityHashMap<>());for(ProviderInfo info:selected)for(String authority:info.authority.split(";")){
                IContentProvider endpoint=process.acquireExistingProvider(context,authority,context.getUserId(),true);
                ContentProvider provider=endpoint==null?null:ContentProvider.coerceToLocalContentProvider(endpoint);
                if(provider!=null)installed.add(provider);
            }installedCount=installed.size();}
        }catch(RuntimeException|Error error){try{close();}catch(Throwable cleanup){if(cleanup!=error)error.addSuppressed(cleanup);}throw error;}
    }
    public ContentProvider require(String authority){requireOwner();if(closed)throw new IllegalStateException("Provider set closed");IContentProvider endpoint=acquire(authority);ContentProvider result=endpoint==null?null:ContentProvider.coerceToLocalContentProvider(endpoint);if(result==null)throw new UnsupportedOperationException("No local Android provider: "+authority);return result;}
    /** Application workers may query providers; lifecycle remains on its owner. */
    public IContentProvider acquire(String authority){return closed||process==null?null:process.acquireExistingProvider(context,authority,context.getUserId(),true);}
    public int size(){return installedCount;}
    private void requireOwner(){if(Thread.currentThread()!=owner)throw new IllegalStateException("Provider lifecycle must run on its owning thread");}
    @Override public void close(){
        requireOwner();if(closed)return;closed=true;
        // These are borrowed from ActivityThread, whose published endpoints may
        // still be held by clients. ContentProvider.shutdown is a test-fixture
        // operation, not a production provider-retirement callback.
        installedCount=0;process=null;context=null;
        if(contentService!=null){maclauncher.android.framework.HostServices.unregister(ContentResolver.CONTENT_SERVICE_NAME,contentService,"android.content.IContentService");contentService.close();contentService=null;}
    }
}
