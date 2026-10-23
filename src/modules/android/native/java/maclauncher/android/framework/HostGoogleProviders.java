package maclauncher.android.framework;

import android.content.*;
import android.content.pm.*;
import android.app.ContentProviderHolder;
import android.net.Uri;
import android.os.Bundle;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.RemoteException;
import maclauncher.android.HostDigests;
import java.util.*;

/** Explicit compatibility identity for the installed, cryptographically verified microG providers. */
public final class HostGoogleProviders {
    private HostGoogleProviders(){}
    private static final String PUBLISHER="9bd06727e62796c0130eb6dab39b73157451582cbd138e86c468acc395d14165";
    private static final String COMPATIBLE="f0fd6c5b410f25cb25c3b53346c8972fae30f8ee7411df910480ad6b2d60db83";
    private static Boolean enabled;
    private static boolean registrationScheduled;
    private static boolean accountInteraction=true;
    public static void accountInteraction(boolean value){accountInteraction=value;}
    private static final Set<String> ACCOUNT_ACTIONS=Set.of("com.google.android.gms.auth.login.LOGIN","com.google.android.gms.auth.GOOGLE_SIGN_IN","com.google.android.gms.common.account.CHOOSE_ACCOUNT","com.google.android.gms.common.account.CHOOSE_ACCOUNT_USERTILE");
    static boolean accountActivityAllowed(ActivityInfo info){
        if(accountInteraction||!applies(info.packageName))return true;
        return Collections.disjoint(HostPackages.activityActions(new ComponentName(info.packageName,info.name)),ACCOUNT_ACTIONS);
    }
    private static final String SERVICE_BROKER="com.google.android.gms.common.internal.IGmsServiceBroker";
    private static final String SERVICE_CALLBACKS="com.google.android.gms.common.internal.IGmsCallbacks";
    private static final String GAMES_CONNECT="com.google.android.gms.games.internal.connect.IGamesConnectService";
    private static final String GAMES_CALLBACKS="com.google.android.gms.games.internal.connect.IGamesConnectCallbacks";
    /** The original broker retains sign-in-required status. A background caller
     * receives no interactive resolution capability; successful service binding
     * and the explicit account owner's original callbacks remain unchanged. */
    static IBinder serviceBinder(Context context,IBinder original){
        if(accountInteraction||original==null||!applies(context.getPackageName()))return original;
        try{
            if(!SERVICE_BROKER.equals(original.getInterfaceDescriptor())||!publisher(HostPackages.verifiedCurrentSigningDetails()))return original;
        }catch(Exception failure){throw new IllegalStateException("Google provider service identity is unavailable",failure);}
        return new ServiceBroker(original,context.getClassLoader());
    }
    private static class ForwardingBinder extends Binder {
        final IBinder original;
        ForwardingBinder(IBinder original,String descriptor){this.original=original;attachInterface(null,descriptor);}
        @Override public boolean pingBinder(){return original.pingBinder();}
        @Override public boolean isBinderAlive(){return original.isBinderAlive();}
        @Override public void linkToDeath(DeathRecipient recipient,int flags){try{original.linkToDeath(recipient,flags);}catch(RemoteException failure){HostUiAccess.raise(failure);}}
        @Override public boolean unlinkToDeath(DeathRecipient recipient,int flags){return original.unlinkToDeath(recipient,flags);}
        @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{return original.transact(code,data,reply,flags);}
    }
    private static final class ServiceBroker extends ForwardingBinder {
        final ClassLoader loader;
        ServiceBroker(IBinder original,ClassLoader loader){super(original,SERVICE_BROKER);this.loader=loader;}
        @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
            // Every original legacy request and getService/validateAccount has
            // the callbacks Binder first. Other transactions keep original bytes.
            if(!(code>=1&&code<=24||code==42||code==46||code==47))return super.onTransact(code,data,reply,flags);
            int position=data.dataPosition();Parcel request=Parcel.obtain();
            try{
                data.enforceInterface(SERVICE_BROKER);IBinder callbacks=data.readStrongBinder();
                request.writeInterfaceToken(SERVICE_BROKER);request.writeStrongBinder(callbacks==null?null:new ServiceCallbacks(callbacks,loader));
                request.appendFrom(data,data.dataPosition(),data.dataAvail());request.setDataPosition(0);
                return original.transact(code,request,reply,flags);
            }finally{data.setDataPosition(position);request.recycle();}
        }
    }
    private static final class ServiceCallbacks extends ForwardingBinder {
        final ClassLoader loader;
        ServiceCallbacks(IBinder original,ClassLoader loader){super(original,SERVICE_CALLBACKS);this.loader=loader;}
        private Bundle accountResolution(Bundle original){
            if(original==null)return null;
            original.setClassLoader(loader);
            if(!original.containsKey("pendingIntent"))return original;
            Bundle result=new Bundle(original);result.remove("pendingIntent");return result;
        }
        @SuppressWarnings("unchecked") private Parcelable connectionInfo(Parcel data)throws ReflectiveOperationException{
            // Use the provider's original public Parcelable contract and creator.
            // Its fields, features and other metadata retain their original owner.
            Class<?> contract=Class.forName("com.google.android.gms.common.internal.ConnectionInfo",false,loader);
            var creator=(Parcelable.Creator<Parcelable>)contract.getField("CREATOR").get(null);
            Parcelable value=data.readTypedObject(creator);
            if(value!=null){var params=contract.getField("params");Bundle original=(Bundle)params.get(value);Bundle selected=accountResolution(original);if(selected!=original)params.set(value,selected);}
            return value;
        }
        @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
            if(code<1||code>3)return super.onTransact(code,data,reply,flags);
            int position=data.dataPosition();Parcel result=null;
            try{
                data.enforceInterface(SERVICE_CALLBACKS);int status=data.readInt();
                if(status==0&&code!=2){
                    IBinder service=data.readStrongBinder();
                    if(service!=null&&GAMES_CONNECT.equals(service.getInterfaceDescriptor())){
                        result=Parcel.obtain();result.writeInterfaceToken(SERVICE_CALLBACKS);result.writeInt(status);
                        result.writeStrongBinder(new GamesConnectService(service,loader));
                        result.appendFrom(data,data.dataPosition(),data.dataAvail());result.setDataPosition(0);
                        return original.transact(code,result,reply,flags);
                    }
                }
                if(status!=4){data.setDataPosition(position);return super.onTransact(code,data,reply,flags);}
                IBinder service=code==2?null:data.readStrongBinder();result=Parcel.obtain();
                result.writeInterfaceToken(SERVICE_CALLBACKS);result.writeInt(status);
                if(code!=2)result.writeStrongBinder(service);
                if(code==3)try{result.writeTypedObject(connectionInfo(data),0);}
                catch(ReflectiveOperationException failure){throw new IllegalStateException("Google service connection metadata is unavailable",failure);}
                else result.writeTypedObject(accountResolution(data.readTypedObject(Bundle.CREATOR)),0);
                result.appendFrom(data,data.dataPosition(),data.dataAvail());result.setDataPosition(0);
                return original.transact(code,result,reply,flags);
            }finally{data.setDataPosition(position);if(result!=null)result.recycle();}
        }
    }
    /** Games Connect binds successfully before reporting sign-in availability.
     * Its later callback carries a Status resolution rather than broker params. */
    private static final class GamesConnectService extends ForwardingBinder {
        final ClassLoader loader;
        GamesConnectService(IBinder original,ClassLoader loader){super(original,GAMES_CONNECT);this.loader=loader;}
        @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
            // Original AIDL signIn = 1, relative to FIRST_CALL_TRANSACTION.
            if(code!=IBinder.FIRST_CALL_TRANSACTION+1)return super.onTransact(code,data,reply,flags);
            int position=data.dataPosition();Parcel request=Parcel.obtain();
            try{
                data.enforceInterface(GAMES_CONNECT);IBinder callbacks=data.readStrongBinder();
                request.writeInterfaceToken(GAMES_CONNECT);request.writeStrongBinder(callbacks==null?null:new GamesConnectCallbacks(callbacks,loader));
                request.appendFrom(data,data.dataPosition(),data.dataAvail());request.setDataPosition(0);
                return original.transact(code,request,reply,flags);
            }finally{data.setDataPosition(position);request.recycle();}
        }
    }
    private static final class GamesConnectCallbacks extends ForwardingBinder {
        final ClassLoader loader;
        GamesConnectCallbacks(IBinder original,ClassLoader loader){super(original,GAMES_CALLBACKS);this.loader=loader;}
        @SuppressWarnings("unchecked") private Parcelable signInStatus(Parcel data)throws ReflectiveOperationException{
            Class<?> contract=Class.forName("com.google.android.gms.common.api.Status",false,loader);
            var creator=(Parcelable.Creator<Parcelable>)contract.getField("CREATOR").get(null);
            Parcelable status=data.readTypedObject(creator);
            if(status==null||(Integer)contract.getMethod("getStatusCode").invoke(status)!=4||contract.getMethod("getResolution").invoke(status)==null)return null;
            // This is the original CREATOR's parcel-owned copy, never the
            // provider's original Status. Keep its version and every other
            // field while removing only the account UI capability.
            var resolution=contract.getDeclaredField("resolution");resolution.setAccessible(true);resolution.set(status,null);
            return status;
        }
        @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
            if(code!=IBinder.FIRST_CALL_TRANSACTION+1)return super.onTransact(code,data,reply,flags);
            int position=data.dataPosition();Parcel result=null;
            try{
                data.enforceInterface(GAMES_CALLBACKS);Parcelable status;
                try{status=signInStatus(data);}
                catch(ReflectiveOperationException failure){throw new IllegalStateException("Google Games sign-in status is unavailable",failure);}
                if(status==null){data.setDataPosition(position);return super.onTransact(code,data,reply,flags);}
                result=Parcel.obtain();result.writeInterfaceToken(GAMES_CALLBACKS);result.writeTypedObject(status,0);
                result.appendFrom(data,data.dataPosition(),data.dataAvail());result.setDataPosition(0);
                return original.transact(code,result,reply,flags);
            }finally{data.setDataPosition(position);if(result!=null)result.recycle();}
        }
    }
    public static synchronized void configure(boolean value){if(enabled!=null&&enabled!=value)throw new IllegalStateException("Google provider policy is already configured");enabled=value;}
    static synchronized boolean applies(String name){return Boolean.TRUE.equals(enabled)&&(name.equals("com.google.android.gms")||name.equals("com.android.vending"));}
    /** Recover before publication; only the verified settings process owns this preference. */
    public static AutoCloseable libraryOperation(Context context,List<ProviderInfo> providers,boolean acquire)throws Exception{
        if(!applies(context.getPackageName())||!context.getPackageName().equals("com.google.android.gms")){
            if(acquire)throw new SecurityException("Google library operations require the installed Google provider");
            return null;
        }
        String authority=context.getPackageName()+".microg.settings";
        if(providers.stream().noneMatch(info->info.authority!=null&&Arrays.asList(info.authority.split(";")).contains(authority)))return null;
        var journal=new java.io.File(context.getNoBackupFilesDir(),"maclauncher-play-library.preference");
        if(!acquire&&!new android.util.AtomicFile(journal).exists())return null;
        if(!publisher(HostPackages.verifiedCurrentSigningDetails())){
            if(acquire)throw new SecurityException("Google library provider signature is not supported");
            return null;
        }
        Class<?> contract=Class.forName("org.microg.gms.settings.SettingsContract$Vending",true,context.getClassLoader());
        String key=(String)contract.getField("LICENSING_PURCHASE_FREE_APPS").get(null);
        return ProviderPreferenceLease.open(android.preference.PreferenceManager.getDefaultSharedPreferences(context),key,
            journal,acquire);
    }
    /** Apply the launcher opt-in at the verified provider's configuration boundary. */
    static void published(Context context,List<ContentProviderHolder> providers){
        if(!applies(context.getPackageName())||!context.getPackageName().equals("com.google.android.gms"))return;
        String authority=context.getPackageName()+".microg.settings";
        for(var holder:providers){
            if(holder.info.authority==null||!Arrays.asList(holder.info.authority.split(";")).contains(authority))continue;
            try{
                if(!publisher(HostPackages.verifiedCurrentSigningDetails()))return;
                // These are the installed provider's own contract and preferences.
                // Its original update handles notification and cross-process access.
                Class<?> contract=Class.forName("org.microg.gms.settings.SettingsContract$Vending",true,context.getClassLoader());
                var preferences=android.preference.PreferenceManager.getDefaultSharedPreferences(context);
                ContentValues defaults=new ContentValues();
                for(String field:List.of("LICENSING","ASSET_DELIVERY","SPLIT_INSTALL","BILLING")){
                    String key=(String)contract.getField(field).get(null);
                    if(!preferences.contains(key))defaults.put(key,true);
                }
                if(defaults.size()!=0)holder.provider.update(context.getAttributionSource(),Uri.parse("content://"+authority+"/vending"),defaults,Bundle.EMPTY);
                Class<?> checkin=Class.forName("org.microg.gms.settings.SettingsContract$CheckIn",true,context.getClassLoader());
                String registration=(String)checkin.getField("ENABLED").get(null);
                if(!preferences.contains(registration)){
                    ContentValues value=new ContentValues();value.put(registration,true);
                    holder.provider.update(context.getAttributionSource(),Uri.parse("content://"+authority+"/check-in"),value,Bundle.EMPTY);
                }
                if(preferences.getBoolean(registration,false)&&!registrationScheduled){
                    // The provider owns registration, freshness, network callbacks
                    // and retry alarms. Re-enter its normal lifecycle after the
                    // original Application and settings provider finish publishing.
                    registrationScheduled=true;
                    new android.os.Handler(context.getMainLooper()).post(()->context.sendOrderedBroadcast(
                        new Intent().setComponent(new ComponentName(context.getPackageName(),"org.microg.gms.checkin.TriggerReceiver")),null));
                }
            }catch(Exception failure){System.err.println("[android-host] Google provider configuration unavailable: "+failure);}
        }
    }
    private static boolean publisher(SigningDetails verified)throws Exception{Signature[] actual=verified.getSignatures();return actual!=null&&actual.length==1&&digest(actual[0]).equals(PUBLISHER);}
    static PackageInfo describe(Context context,PackageInfo info,Bundle metadata,SigningDetails verified)throws Exception{
        if(info==null||!applies(info.packageName)||info.signatures==null&&info.signingInfo==null)return info;
        if(!publisher(verified)||metadata==null)return info;
        Object declared=metadata.get("fake-signature");
        String text=declared instanceof String value?value:declared instanceof Integer resource?context.getPackageManager().getResourcesForApplication(info.applicationInfo).getString(resource):null;
        if(text==null)return info;
        Signature compatible=new Signature(text);
        if(!digest(compatible).equals(COMPATIBLE))return info;
        // Only this newly generated query result changes. The parsed signer,
        // signing lineage, permission checks and APK bytes remain the publisher's.
        if(info.signatures!=null)info.signatures=new Signature[]{compatible};
        if(info.signingInfo!=null)info.signingInfo=new SigningInfo(new SigningDetails(new Signature[]{compatible},verified.getSignatureSchemeVersion()));
        return info;
    }
    private static String digest(Signature signature)throws Exception{return HexFormat.of().formatHex(HostDigests.sha256().digest(signature.toByteArray()));}
}
