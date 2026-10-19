package com.android.server.am;

import android.Manifest;
import android.app.ActivityManager;
import android.app.AppOpsManager;
import android.app.IUidFrozenStateChangedCallback;
import android.app.IUidObserver;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.*;
import java.util.Objects;
import maclauncher.android.framework.HostActivityManager;

/** Original UID observer controller driven by the existing process lifecycle. */
public final class HostUidObservers implements AutoCloseable {
    private final Context context;
    private final Handler handler;
    private final UidObserverController controller;
    private final RemoteCallbackList<IUidFrozenStateChangedCallback> frozen=new RemoteCallbackList<>();
    private final java.util.function.BiConsumer<Integer,Integer> stateChanged=this::stateChanged;
    private long sequence;
    private volatile boolean closed;
    public HostUidObservers(Context context){
        this.context=context;
        handler=new Handler(context.getMainLooper()){
            @Override public void handleMessage(Message message){
                if(closed)return;
                if(message.what==ActivityManagerService.ADD_UID_TO_OBSERVER_MSG)controller.addUidToObserverImpl((IBinder)message.obj,message.arg1);
                else if(message.what==ActivityManagerService.REMOVE_UID_FROM_OBSERVER_MSG)controller.removeUidFromObserverImpl((IBinder)message.obj,message.arg1);
                else super.handleMessage(message);
            }
        };
        controller=new UidObserverController(handler);
        HostActivityManager.hostObserveUid(stateChanged);
    }
    private void usage(String packageName){
        int uid=Binder.getCallingUid();
        int mode=context.getSystemService(AppOpsManager.class).noteOpNoThrow(AppOpsManager.OP_GET_USAGE_STATS,uid,packageName);
        if(mode!=AppOpsManager.MODE_ALLOWED)context.enforceCallingOrSelfPermission(Manifest.permission.PACKAGE_USAGE_STATS,"UID observation");
    }
    public IBinder register(IUidObserver observer,int which,int cutpoint,String packageName,int[] uids){
        usage(packageName);Objects.requireNonNull(observer);
        if(closed)throw new IllegalStateException("Process observer owner has stopped");
        return controller.register(observer,which,cutpoint,packageName,Binder.getCallingUid(),uids);
    }
    public void unregister(IUidObserver observer){controller.unregister(Objects.requireNonNull(observer));}
    public void add(IBinder token,String packageName,int uid){usage(packageName);controller.addUidToObserver(Objects.requireNonNull(token),uid);}
    public void remove(IBinder token,String packageName,int uid){usage(packageName);controller.removeUidFromObserver(Objects.requireNonNull(token),uid);}
    public void registerFrozen(IUidFrozenStateChangedCallback observer){
        context.enforceCallingOrSelfPermission(Manifest.permission.PACKAGE_USAGE_STATS,"registerUidFrozenStateChangedCallback");
        if(closed)throw new IllegalStateException("Process observer owner has stopped");
        frozen.register(Objects.requireNonNull(observer));
    }
    public void unregisterFrozen(IUidFrozenStateChangedCallback observer){
        context.enforceCallingOrSelfPermission(Manifest.permission.PACKAGE_USAGE_STATS,"unregisterUidFrozenStateChangedCallback");
        frozen.unregister(Objects.requireNonNull(observer));
    }
    public int[] frozen(int[] uids){
        context.enforceCallingOrSelfPermission(Manifest.permission.PACKAGE_USAGE_STATS,"getUidFrozenState");
        int[] result=new int[Objects.requireNonNull(uids).length];
        // There is no Android cgroup freezer in the host process owner. As in
        // ActivityManagerService, unknown and running UIDs are both unfrozen.
        java.util.Arrays.fill(result,ActivityManager.UidFrozenStateChangedCallback.UID_FROZEN_STATE_UNFROZEN);
        return result;
    }
    private void stateChanged(int uid,int state){
        handler.post(()->{
            if(closed)return;
            int flags=UidRecord.CHANGE_PROCSTATE|(state==ActivityManager.PROCESS_STATE_NONEXISTENT?UidRecord.CHANGE_GONE:ActivityManager.isProcStateCached(state)?UidRecord.CHANGE_CACHED:UidRecord.CHANGE_UNCACHED);
            controller.enqueueUidChange(null,uid,flags,state,ProcessList.INVALID_ADJ,++sequence,ActivityManager.PROCESS_CAPABILITY_NONE,false);
        });
    }
    @Override public void close(){
        if(closed)return;closed=true;
        HostActivityManager.hostRetireUidObserver(stateChanged);
        handler.removeCallbacksAndMessages(null);controller.mUidObservers.kill();frozen.kill();
    }
}
