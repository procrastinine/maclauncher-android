package maclauncher.android.framework;
import com.android.internal.pm.parsing.pkg.ParsedPackage;
import java.nio.file.Path;
import java.nio.file.Files;
import java.util.List;
/** Original current package parser with host archive and device policy. */
final class HostPackageParser {
 private HostPackageParser(){}
 static ParsedPackage parseSources(List<Path> sources,Path cache)throws Exception{
  if(sources.isEmpty())throw new IllegalArgumentException("Android package has no source APK");Path cluster=null;
  try{
   Path input=sources.getFirst();if(sources.size()>1){cluster=Files.createTempDirectory(cache,"package-manifest-");for(int i=0;i<sources.size();i++)Files.createSymbolicLink(cluster.resolve(i+".apk"),sources.get(i).toRealPath());input=cluster;}
   ParsedPackage parsed=parse(input,0);String base=Path.of(parsed.getBaseApkPath()).toRealPath().toString();String[] splits=parsed.getSplitCodePaths()==null?null:parsed.getSplitCodePaths().clone();if(splits!=null)for(int i=0;i<splits.length;i++)splits[i]=Path.of(splits[i]).toRealPath().toString();
   parsed.setPath(Path.of(base).getParent().toString()).setBaseApkPath(base).setSplitCodePaths(splits);return parsed;
  }finally{if(cluster!=null){try(var children=Files.list(cluster)){for(Path child:children.toList())Files.deleteIfExists(child);}Files.deleteIfExists(cluster);}}
 }
 static ParsedPackage parse(Path source,int flags)throws com.android.internal.pm.parsing.PackageParserException{
  var metrics=new android.util.DisplayMetrics();HostDisplay.metrics(metrics);
  return PackageParsing.parse(source,flags,metrics);
 }
}
