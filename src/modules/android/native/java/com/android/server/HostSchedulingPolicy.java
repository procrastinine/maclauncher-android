package com.android.server;

import android.os.ISchedulingPolicyService;
import android.os.ServiceManager;
import com.android.server.os.SchedulingPolicyService;
import maclauncher.android.framework.HostServices;

/** Publishes the original system scheduling service, including its permission
 * checks, native Process calls and Binder death ownership. */
public final class HostSchedulingPolicy implements AutoCloseable {
    private static final String NAME="scheduling_policy";
    private final SchedulingPolicyService service;
    private boolean closed;
    public HostSchedulingPolicy(){
        SystemServerInitThreadPool.start();
        try {service=new SchedulingPolicyService();}
        finally {SystemServerInitThreadPool.shutdown();}
        ServiceManager.addService(NAME,service);
    }
    @Override public void close(){
        if(closed)return;closed=true;
        HostServices.unregister(NAME,service,ISchedulingPolicyService.class.getName());
    }
}
