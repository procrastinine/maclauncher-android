package maclauncher.android.framework;
import android.content.pm.*;
import android.content.pm.parsing.result.ParseInput;
import android.util.DisplayMetrics;
import com.android.internal.pm.parsing.PackageParser2;
import com.android.internal.pm.parsing.pkg.ParsedPackage;
import java.nio.file.Path;
import java.util.Objects;
import java.util.Set;
/** Original package parser with common host device policy and supplied display metrics. */
public final class PackageParsing {
 private PackageParsing(){}
 public static ParsedPackage parse(Path source,int flags,DisplayMetrics metrics)throws com.android.internal.pm.parsing.PackageParserException{
  Objects.requireNonNull(source);Objects.requireNonNull(metrics);
  try(var parser=new PackageParser2(null,metrics,null,new PackageParser2.Callback(){
   @Override public boolean hasFeature(String feature){return HostActivityManager.hostHasSystemFeature(feature,0);}
   @Override public boolean isChangeEnabled(long id,ApplicationInfo info){
    // Native AssetManager supports compressed tables in APKs and the framework archive.
    if(id==ParseInput.DeferredError.RESOURCES_ARSC_COMPRESSED)return false;
    int threshold=ParseInput.DeferredError.getTargetSdkForChange(id);
    return threshold<0?android.compat.Compatibility.isChangeEnabled(id):info.targetSdkVersion>threshold;
   }
   @Override public Set<String> getHiddenApiWhitelistedApps(){return Set.of();}
   @Override public Set<String> getInstallConstraintsAllowlist(){return Set.of();}
  })){return parser.parsePackage(source.toFile(),flags,false);}
 }
}
