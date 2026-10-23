package com.android.server.wm;

import android.os.LocaleList;
import java.util.Objects;
import java.util.function.BiConsumer;

/** Process lifetime for the original package configuration serializer and queue. */
public final class HostPackageConfiguration implements AutoCloseable {
    private static HostPackageConfiguration current;
    private final PersisterQueue queue=new PersisterQueue();
    private final PackageConfigPersister persister=new PackageConfigPersister(queue,null);
    private final int user;
    private final BiConsumer<String,Integer> changed;
    private boolean closed,persisting;
    public HostPackageConfiguration(int user,BiConsumer<String,Integer> changed){
        this.user=user;this.changed=Objects.requireNonNull(changed);
        synchronized(HostPackageConfiguration.class){if(current!=null)throw new IllegalStateException("Android package configuration already has an owner");current=this;}
        try{queue.startPersisting();persisting=true;persister.loadUserPackages(user);}catch(RuntimeException|Error failure){try{close();}catch(Throwable cleanup){failure.addSuppressed(cleanup);}throw failure;}
    }
    private static synchronized HostPackageConfiguration owner(){if(current==null)throw new IllegalStateException("Android package configuration has no owner");return current;}
    private void active(int user){if(closed)throw new IllegalStateException("Android package configuration is closed");if(user!=this.user)throw new IllegalArgumentException("Android user is unavailable: "+user);}
    public static ActivityTaskManagerInternal.PackageConfig get(Object original,String name,int user){
        if(original instanceof ActivityTaskManagerInternal service)return service.getApplicationConfig(name,user);
        HostPackageConfiguration owner=owner();synchronized(owner){owner.active(user);return owner.persister.findPackageConfiguration(Objects.requireNonNull(name),user);}
    }
    public static ActivityTaskManagerInternal.PackageConfigurationUpdater create(Object original,String name,int user){
        if(original instanceof ActivityTaskManagerInternal service)return service.createPackageConfigurationUpdater(name,user);
        HostPackageConfiguration owner=owner();synchronized(owner){owner.active(user);}
        return new ActivityTaskManagerInternal.PackageConfigurationUpdater(){
            final PackageConfigurationUpdaterImpl update=new PackageConfigurationUpdaterImpl(Objects.requireNonNull(name),user,null);
            public ActivityTaskManagerInternal.PackageConfigurationUpdater setNightMode(int value){update.setNightMode(value);return this;}
            public ActivityTaskManagerInternal.PackageConfigurationUpdater setLocales(LocaleList value){update.setLocales(value);return this;}
            public ActivityTaskManagerInternal.PackageConfigurationUpdater setGrammaticalGender(int value){update.setGrammaticalGender(value);return this;}
            public boolean commit(){boolean committed;synchronized(owner){owner.active(user);committed=owner.persister.updateFromImpl(name,user,update);if(committed)owner.queue.flush();}if(committed)owner.changed.accept(name,user);return committed;}
        };
    }
    public static LocaleList combine(LocaleList overlay,LocaleList system){return LocaleOverlayHelper.combineLocalesIfOverlayExists(overlay,system);}
    public synchronized void packageRemoved(String name,int user){active(user);persister.onPackageUninstall(name,user);queue.flush();}
    public synchronized void packageDataCleared(String name,int user){active(user);persister.onPackageDataCleared(name,user);queue.flush();}
    @Override public void close(){synchronized(this){if(closed)return;closed=true;if(persisting)queue.flush();}try{queue.stopPersisting();}catch(InterruptedException failure){Thread.currentThread().interrupt();throw new IllegalStateException("Android package configuration writer was interrupted",failure);}finally{synchronized(HostPackageConfiguration.class){if(current==this)current=null;}}}
}
