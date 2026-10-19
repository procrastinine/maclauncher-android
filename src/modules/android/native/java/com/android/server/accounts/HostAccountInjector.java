package com.android.server.accounts;

import android.content.Context;
import android.os.Looper;

/** Retains the original account-service worker and exposes its host lifetime. */
public final class HostAccountInjector extends AccountManagerService.Injector {
    private Looper worker;
    public HostAccountInjector(Context context){super(context);}
    @Override Looper getMessageHandlerLooper(){return worker=super.getMessageHandlerLooper();}
    public void closeWorker(){if(worker==null)return;worker.quitSafely();if(worker.getThread()!=Thread.currentThread())try{worker.getThread().join();}catch(InterruptedException interrupted){Thread.currentThread().interrupt();}}
}
