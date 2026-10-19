package maclauncher.android.framework;

import android.content.Intent;
import android.content.res.*;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.util.DisplayMetrics;
import com.android.internal.pm.parsing.pkg.AndroidPackageHidden;
import java.nio.file.*;
import java.util.*;
import org.json.JSONObject;

/** Original package parsing and drawable rendering, without loading application code. */
public final class PackageMetadata {
    private PackageMetadata() {}
    public static void initialize(Path runtime)throws Exception {
        Class.forName("libcore.util.NativeAllocationRegistry");
        AssetManager.createSystemAssetsInZygoteLocked(false,runtime.resolve("android-framework.jar").toRealPath().toString());
    }
    public static JSONObject read(List<Path> sources,Path cache,String languageTag,Path icon)throws Exception {
        var parsed=HostPackageParser.parseSources(sources,cache);
        var application=((AndroidPackageHidden)parsed).toAppInfoWithoutState();
        var activityWindows=new JSONObject();
        for(var activity:parsed.getActivities()){
            var info=new android.content.pm.ActivityInfo();info.screenOrientation=activity.getScreenOrientation();
            Boolean landscape=HostActivity.initialWindowLandscape(info);
            activityWindows.put(activity.getName(),new JSONObject().put("landscape",landscape==null?JSONObject.NULL:landscape));
        }
        int label=application.labelRes,image=application.icon;
        CharSequence literal=application.nonLocalizedLabel;
        // The same launcher component that Android presents may override its app's identity.
        for(var activity:parsed.getActivities()) {
            if(!activity.isEnabled())continue;
            boolean launcher=activity.getIntents().stream().anyMatch(value->value.getIntentFilter().hasAction(Intent.ACTION_MAIN)&&value.getIntentFilter().hasCategory(Intent.CATEGORY_LAUNCHER));
            if(!launcher)continue;
            if(activity.getLabelRes()!=0||activity.getNonLocalizedLabel()!=null){label=activity.getLabelRes();literal=activity.getNonLocalizedLabel();}
            if(activity.getIcon()!=0)image=activity.getIcon();
            break;
        }
        var owned=new ArrayList<ApkAssets>();
        try {
            var builder=new AssetManager.Builder();
            owned.add(ApkAssets.loadFromPath(parsed.getBaseApkPath()));
            if(parsed.getSplitCodePaths()!=null)for(String file:parsed.getSplitCodePaths())owned.add(ApkAssets.loadFromPath(file));
            for(var asset:owned)builder.addApkAssets(asset);
            try(var assets=builder.build()) {
                var metrics=new DisplayMetrics();metrics.setToDefaults();metrics.densityDpi=DisplayMetrics.DENSITY_XXXHIGH;metrics.density=metrics.scaledDensity=4;
                var config=new Configuration();config.setToDefaults();config.setLocale(Locale.forLanguageTag(languageTag));config.densityDpi=metrics.densityDpi;
                var resources=new Resources(assets,metrics,config);
                var result=new JSONObject().put("packageName",parsed.getPackageName()).put("activityWindows",activityWindows);
                if(literal==null&&label!=0)try{literal=resources.getText(label);}catch(Resources.NotFoundException unavailable){result.put("labelDiagnostic",unavailable.toString());}
                result.put("label",literal==null?parsed.getPackageName():literal.toString().strip());
                if(image!=0)try {
                    Drawable drawable=resources.getDrawableForDensity(image,DisplayMetrics.DENSITY_XXXHIGH,null);
                    Bitmap bitmap=Bitmap.createBitmap(256,256,Bitmap.Config.ARGB_8888);
                    try {drawable.setBounds(0,0,256,256);drawable.draw(new Canvas(bitmap));try(var output=Files.newOutputStream(icon)){if(!bitmap.compress(Bitmap.CompressFormat.PNG,100,output))throw new java.io.IOException("Android icon could not be encoded");}result.put("icon",true);}
                    finally{bitmap.recycle();}
                }catch(Resources.NotFoundException unavailable){result.put("iconDiagnostic",unavailable.toString());}
                return result;
            }
        }finally{for(var asset:owned)asset.close();}
    }
}
