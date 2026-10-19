package maclauncher.android;

import android.content.*;
import android.os.*;
import java.security.SecureRandom;
import java.util.concurrent.*;

/** Client of the original public Google Play LVL protocol; the provider owns acquisition. */
final class PlayLibraryOperation implements AutoCloseable {
    static final String SERVICE="com.android.vending.licensing.ILicensingService";
    static final String LISTENER="com.android.vending.licensing.ILicenseResultListener";
    record Result(String status,int code){}
    private final Context context;
    private final CompletableFuture<Result> result=new CompletableFuture<>();
    private final Handler main;
    private IBinder remote;
    private boolean bound,closed;
    private final IBinder.DeathRecipient death=()->result.complete(new Result("unavailable",-1));
    private final ServiceConnection connection=new ServiceConnection(){
        @Override public void onServiceConnected(ComponentName name,IBinder service){
            if(closed)return;
            remote=service;
            try{service.linkToDeath(death,0);request(service,context.getPackageName(),new SecureRandom().nextLong(),result);}
            catch(RemoteException|RuntimeException failure){result.complete(new Result("failure",-1));}
        }
        @Override public void onServiceDisconnected(ComponentName name){result.complete(new Result("unavailable",-1));}
        @Override public void onBindingDied(ComponentName name){result.complete(new Result("unavailable",-1));}
        @Override public void onNullBinding(ComponentName name){result.complete(new Result("unavailable",-1));}
    };
    private final Runnable timeout=()->result.complete(new Result("timeout",-1));
    PlayLibraryOperation(Context context,ProcessAuthority authority,String id){
        this.context=context;main=new Handler(context.getMainLooper());
        result.whenComplete((value,error)->main.post(()->{
            close();
            try{authority.operationResult(id,value.status(),value.code());}
            catch(Exception ignored){/* A disconnected supervisor already owns teardown. */}
        }));
        main.post(()->{
            if(closed)return;
            try{bound=context.bindService(new Intent(SERVICE).setPackage("com.android.vending"),connection,Context.BIND_AUTO_CREATE);
                if(!bound)result.complete(new Result("unavailable",-1));else main.postDelayed(timeout,120000);
            }catch(RuntimeException failure){result.complete(new Result("failure",-1));}
        });
    }
    static void request(IBinder service,String packageName,long nonce,CompletableFuture<Result> result)throws RemoteException{
        Binder callback=new Binder(){
            @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
                if(code==INTERFACE_TRANSACTION){if(reply!=null)reply.writeString(LISTENER);return true;}
                if(code!=FIRST_CALL_TRANSACTION)return super.onTransact(code,data,reply,flags);
                data.enforceInterface(LISTENER);int response=data.readInt();
                // Signed response and signature belong to the license consumer.
                // The launcher does not retain, reinterpret or fabricate them.
                data.readString();data.readString();data.enforceNoDataAvail();
                result.complete(new Result(response==0||response==2?"licensed":response==1?"not-licensed":"failure",response));return true;
            }
        };
        try(ParcelHolder parcel=new ParcelHolder()){
            parcel.data.writeInterfaceToken(SERVICE);parcel.data.writeLong(nonce);parcel.data.writeString(packageName);parcel.data.writeStrongBinder(callback);
            if(!service.transact(IBinder.FIRST_CALL_TRANSACTION,parcel.data,null,IBinder.FLAG_ONEWAY))result.complete(new Result("unavailable",-1));
        }
    }
    private static final class ParcelHolder implements AutoCloseable{final Parcel data=Parcel.obtain();public void close(){data.recycle();}}
    @Override public void close(){
        if(closed)return;closed=true;main.removeCallbacks(timeout);
        if(remote!=null)try{remote.unlinkToDeath(death,0);}catch(RuntimeException ignored){}
        if(bound)try{context.unbindService(connection);}catch(RuntimeException ignored){}
    }
}
