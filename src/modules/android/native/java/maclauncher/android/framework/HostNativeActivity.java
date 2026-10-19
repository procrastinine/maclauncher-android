package maclauncher.android.framework;

import android.app.NativeActivity;
import android.content.res.AssetManager;
import android.os.*;
import android.view.Surface;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import maclauncher.android.bridge.*;

/** Existing Klepton NDK owner beneath the original NativeActivity Java lifecycle. */
public final class HostNativeActivity {
    private static final Map<NativeActivity,Owner> owners=new IdentityHashMap<>();
    private static long nextHandle;
    private static final class Owner {
        final NativeActivity activity;final long id;final Handler requests;
        final Runnable apply=this::apply;
        GuestNativeLoader.ActivityHandle nativeOwner;
        long window;boolean closed,queued;int pending,format,flags,mask,keyboard,keyboardFlags;
        Owner(NativeActivity activity){this.activity=activity;id=++nextHandle;requests=new Handler(Looper.getMainLooper());}
        synchronized void request(int kind,int first,int second){
            if(closed)return;
            switch(kind){case 0->pending|=8;case 1->{pending|=1;format=first;}case 2->{pending|=2;int changed=first|second;flags=(flags&~changed)|first;mask|=changed;}case 3,4->{pending|=4;keyboard=kind;keyboardFlags=first;}default->throw new IllegalArgumentException("Unknown native window request");}
            if(!queued){queued=requests.post(apply);if(!queued)throw new IllegalStateException("Native Activity owner Looper has closed");}
        }
        void apply(){
            int work,nextFormat,nextFlags,nextMask,nextKeyboard,nextKeyboardFlags;
            synchronized(this){queued=false;work=closed?0:pending;nextFormat=format;nextFlags=flags;nextMask=mask;nextKeyboard=keyboard;nextKeyboardFlags=keyboardFlags;pending=flags=mask=0;}
            if((work&1)!=0)invoke("setWindowFormat",new Class<?>[]{int.class},nextFormat);
            if((work&2)!=0)invoke("setWindowFlags",new Class<?>[]{int.class,int.class},nextFlags,nextMask);
            if((work&4)!=0)invoke(nextKeyboard==3?"showIme":"hideIme",new Class<?>[]{int.class},nextKeyboardFlags);
            if((work&8)!=0)activity.finish();
        }
        void invoke(String method,Class<?>[] types,Object...args){HostUiAccess.call(HostUiAccess.method(NativeActivity.class,method,types),activity,args);}
        synchronized void closeRequests(){closed=true;requests.removeCallbacks(apply);pending=flags=mask=0;queued=false;}
    }
    public static long loadNativeCode(NativeActivity activity,String path,String entry,MessageQueue queue,String files,String obb,String external,int sdk,AssetManager assets,byte[] saved,ClassLoader loader,String search){
        Objects.requireNonNull(activity);Objects.requireNonNull(queue);Objects.requireNonNull(assets);
        if(!Looper.getMainLooper().isCurrentThread())throw new IllegalStateException("Native Activity creation requires the main Looper");
        String name=Path.of(path).getFileName().toString();if(!name.startsWith("lib")||!name.endsWith(".so"))throw new IllegalArgumentException("Android native library name is invalid");
        Owner owner;
        synchronized(owners){if(!owners.isEmpty())throw new IllegalStateException("A native Activity already owns this process");owner=new Owner(activity);owners.put(activity,owner);}
        try{owner.nativeOwner=GuestNativeLoader.createActivity(name.substring(3,name.length()-3),entry,activity,new byte[][]{bytes(files),bytes(external),bytes(obb)},sdk,saved);return owner.id;}
        catch(Throwable failure){owner.closeRequests();synchronized(owners){owners.remove(activity);}throw failure;}
    }
    private static byte[] bytes(String value){return value==null?null:value.getBytes(StandardCharsets.UTF_8);}
    private static Owner owner(NativeActivity activity,long handle){synchronized(owners){Owner owner=owners.get(activity);if(owner==null||owner.id!=handle||owner.closed)throw new IllegalStateException("Native Activity has no live native owner");return owner;}}
    /** Called by the existing ANativeActivity callbacks, including callbacks from guest workers. */
    public static void request(NativeActivity activity,int kind,int first,int second){Owner owner;synchronized(owners){owner=owners.get(activity);}if(owner!=null)owner.request(kind,first,second);}
    private static byte[] event(NativeActivity activity,long handle,int kind,long argument){return owner(activity,handle).nativeOwner.event(kind,argument);}
    public static String getDlError(NativeActivity activity){return "Native library loading did not return an owner";}
    public static void unloadNativeCode(NativeActivity activity,long handle){
        Owner owner=owner(activity,handle);Throwable first=null;
        try{if(owner.window!=0)onSurfaceDestroyedNative(activity,handle);}catch(Throwable failure){first=failure;}
        owner.closeRequests();synchronized(owners){owners.remove(activity);}
        try{owner.nativeOwner.event(6,0);}catch(Throwable failure){if(first==null)first=failure;else if(first!=failure)first.addSuppressed(failure);}
        if(first!=null)HostUiAccess.raise(first);
    }
    public static void onStartNative(NativeActivity activity,long handle){event(activity,handle,1,0);}
    public static void onResumeNative(NativeActivity activity,long handle){event(activity,handle,2,0);}
    public static byte[] onSaveInstanceStateNative(NativeActivity activity,long handle){return event(activity,handle,5,0);}
    public static void onPauseNative(NativeActivity activity,long handle){event(activity,handle,3,0);}
    public static void onStopNative(NativeActivity activity,long handle){event(activity,handle,4,0);}
    public static void onConfigurationChangedNative(NativeActivity activity,long handle){event(activity,handle,12,0);}
    public static void onLowMemoryNative(NativeActivity activity,long handle){event(activity,handle,13,0);}
    public static void onWindowFocusChangedNative(NativeActivity activity,long handle,boolean focused){event(activity,handle,7,focused?1:0);}
    public static void onSurfaceCreatedNative(NativeActivity activity,long handle,Surface surface){Owner owner=owner(activity,handle);if(owner.window!=0)throw new IllegalStateException("Native Activity window already exists");owner.window=NativeSurface.acquire(surface);if(owner.window==0)throw new IllegalStateException("Native Activity surface has no native window");event(activity,handle,8,owner.window);}
    public static void onSurfaceChangedNative(NativeActivity activity,long handle,Surface surface,int format,int width,int height){Owner owner=owner(activity,handle);if(owner.window!=0)event(activity,handle,9,owner.window);}
    public static void onSurfaceRedrawNeededNative(NativeActivity activity,long handle,Surface surface){Owner owner=owner(activity,handle);if(owner.window!=0)event(activity,handle,10,owner.window);}
    public static void onSurfaceDestroyedNative(NativeActivity activity,long handle){Owner owner=owner(activity,handle);long window=owner.window;if(window==0)return;owner.window=0;try{event(activity,handle,11,window);}finally{NativeSurface.releaseWindow(window);}}
    public static void onInputQueueCreatedNative(NativeActivity activity,long handle,long queue){event(activity,handle,14,queue);}
    public static void onInputQueueDestroyedNative(NativeActivity activity,long handle,long queue){event(activity,handle,15,queue);}
    public static void onContentRectChangedNative(NativeActivity activity,long handle,int x,int y,int width,int height){owner(activity,handle).nativeOwner.contentRect(x,y,width,height);}
}
