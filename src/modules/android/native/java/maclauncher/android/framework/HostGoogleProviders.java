package maclauncher.android.framework;

import android.content.*;
import android.content.pm.*;
import android.app.ContentProviderHolder;
import android.net.Uri;
import android.os.Bundle;
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
