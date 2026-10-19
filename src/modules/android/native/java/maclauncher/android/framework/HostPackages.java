package maclauncher.android.framework;

import android.content.*;
import android.content.pm.*;
import com.android.internal.pm.parsing.pkg.*;
import com.android.internal.pm.pkg.component.*;
import com.android.server.pm.PackageSetting;
import com.android.server.pm.PackageProperty;
import com.android.server.pm.pkg.PackageUserStateInternal;
import com.android.server.pm.pkg.PackageUserStateUtils;
import com.android.server.pm.parsing.PackageInfoUtils;
import android.content.res.*;
import android.os.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;

/** Original parsed installations and framework package algorithms behind IPackageManager. */
public final class HostPackages {
 private HostPackages(){}
 private static volatile String name,version;private static int versionCode;
 private static volatile long firstInstallTime,lastUpdateTime;
 private static volatile Context context;
 private static volatile Installation current;
 private static volatile Map<String,Installation> catalog=Map.of();
 private static final Set<String> persistentInstallations=new HashSet<>();
 private static final Map<HostBinderRpc.Peer,Set<String>> peerInstallations=new HashMap<>();
 private static final Map<HostBinderRpc.Peer,String> peerAndroidIds=new HashMap<>();
 private static boolean installed;
 private static final Set<PackageManagerInternal.PackageListObserver> packageObservers=new LinkedHashSet<>();
 static synchronized void addPackageObserver(PackageManagerInternal.PackageListObserver observer){packageObservers.add(Objects.requireNonNull(observer));}
 static synchronized void removePackageObserver(PackageManagerInternal.PackageListObserver observer){packageObservers.remove(observer);}
 private static final com.android.server.pm.PackageMonitorCallbackHelper monitors=new com.android.server.pm.PackageMonitorCallbackHelper();
 private static final IPackageManager endpoint=(IPackageManager)Proxy.newProxyInstance(IPackageManager.class.getClassLoader(),new Class<?>[]{IPackageManager.class},HostPackages::invoke);
 private static final Binder binder=new Binder();
 static{binder.attachInterface(endpoint,"android.content.pm.IPackageManager");}
 public static IPackageManager service(){return endpoint;}
 static String androidId(String packageName){Installation value=catalog.get(packageName);return value==null?null:value.androidId;}
 static synchronized String androidId(HostBinderRpc.Peer peer,String packageName){String value=peer!=null&&peer.packageName().equals(packageName)?peerAndroidIds.get(peer):null;return value==null?androidId(packageName):value;}
 public static SigningDetails verifiedCurrentSigningDetails()throws Exception{Installation value=current;if(value==null)throw new IllegalStateException("Android package is not installed");return value.verifiedSigningDetails();}
 public static SigningDetails verifiedSigningDetails(String pkg)throws Exception{Installation value=catalog.get(pkg);if(value==null)throw new IllegalArgumentException("The installed account provider is unavailable");return value.verifiedSigningDetails();}
 public static IBinder binder(){return binder;}
 public static void configure(String packageName,String versionName,int code){name=Objects.requireNonNull(packageName);version=versionName;versionCode=code;}
 public static synchronized void install(Context owner){context=Objects.requireNonNull(owner);if(!installed){HostServices.register("package",endpoint,"android.content.pm.IPackageManager");installed=true;}}
 public static void hostInstallTimes(Path data,Path source)throws java.io.IOException{
  var dir=Files.readAttributes(data,java.nio.file.attribute.BasicFileAttributes.class);var apk=Files.readAttributes(source,java.nio.file.attribute.BasicFileAttributes.class);
  if(!dir.isDirectory()||!apk.isRegularFile())throw new java.io.IOException("Android installation paths are invalid");
  firstInstallTime=dir.creationTime().toMillis();lastUpdateTime=Math.max(firstInstallTime,apk.lastModifiedTime().toMillis());
 }
 private static ComponentName key(ParsedComponent info){return info.getComponentName();}
 private static Installation find(String pkg,int user){return user==UserHandle.myUserId()?catalog.get(pkg):null;}
 private static Installation find(ComponentName component,int user){return component==null?null:find(component.getPackageName(),user);}
 private static ParsedPackage parse(List<Path> sources)throws Exception{
  return HostPackageParser.parseSources(sources,HostContext.hostDataRoot().resolve("cache"));
 }
 public static synchronized List<ProviderInfo> inspectSources(List<Path> sources)throws Exception{
  return inspectSources(sources,true);
 }
 public static synchronized List<ProviderInfo> inspectSources(List<Path> sources,boolean platformComponents)throws Exception{
  HostPermissions.install();ParsedPackage parsed=parse(sources);if(!Objects.equals(name,parsed.getPackageName()))throw new IllegalArgumentException("APK package differs from its private installation");
  var host=HostContext.hostApplicationInfoSnapshot();Installation local=new Installation(parsed,Path.of(host.dataDir),host.nativeLibraryDir,host.uid,firstInstallTime,lastUpdateTime,host.primaryCpuAbi);
  ApplicationInfo info=local.app(PackageManager.GET_META_DATA|PackageManager.GET_SHARED_LIBRARY_FILES,UserHandle.myUserId());FrameworkResources.adoptPackageSources(info);HostContext.adoptApplicationInfo(info,parsed.getMetaData());
  configure(parsed.getPackageName(),parsed.getVersionName(),(int)parsed.getLongVersionCode());current=local;catalog=Map.of(name,local);persistentInstallations.clear();persistentInstallations.add(name);peerInstallations.clear();peerAndroidIds.clear();HostPermissions.configure(FrameworkResources.frameworkPath(),parsed);
  if(platformComponents)HostPlatformPackages.inspect();
  return parsed.getProviders().stream().map(value->local.<ProviderInfo>component(value,PackageManager.GET_META_DATA|PackageManager.GET_URI_PERMISSION_PATTERNS,UserHandle.myUserId())).filter(Objects::nonNull).toList();
 }
 /** Supplied image bytes and their original manifests own the system component
  * declarations; peer-installed APKs cannot acquire this installation role. */
 static synchronized void inspectPlatform(Path source,String expectedPackage)throws Exception{
  Installation previous=catalog.get(expectedPackage);
  if(previous!=null){if(previous.systemComponent)return;throw new SecurityException("Android platform package identity is already enrolled");}
  ParsedPackage parsed=parse(List.of(source));
  if(!expectedPackage.equals(parsed.getPackageName())||!"android.uid.system".equals(parsed.getSharedUserId()))throw new SecurityException("Android platform manifest differs from its supplied identity");
  Path data=android.os.Process.myUid()==android.os.Process.SYSTEM_UID?HostFilesystem.dataRoot().resolve("packages").resolve(expectedPackage):Path.of("/data/system/packages").resolve(expectedPackage);
  if(android.os.Process.myUid()==android.os.Process.SYSTEM_UID){Files.createDirectories(data);Files.createDirectories(data.resolve("device_protected"));}
  parsed.setSystem(true);Installation value=new Installation(parsed,data,source.getParent().toString(),android.os.Process.SYSTEM_UID,0,0);value.systemComponent=true;
  var next=new LinkedHashMap<>(catalog);next.put(expectedPackage,value);catalog=Collections.unmodifiableMap(next);persistentInstallations.add(expectedPackage);
 }
 static boolean platformComponent(ComponentInfo info){Installation value=catalog.get(info.packageName);return value!=null&&value.systemComponent;}
 static boolean platformPackagePresent(String name){Installation value=catalog.get(name);return value!=null&&value.systemComponent;}
 static List<ProviderInfo> platformProviders(String packageName){Installation value=catalog.get(packageName);if(value==null||!value.systemComponent)return List.of();return value.parsedPackage.getProviders().stream().map(provider->value.<ProviderInfo>component(provider,PackageManager.GET_META_DATA,UserHandle.myUserId())).filter(Objects::nonNull).filter(info->Objects.equals(info.processName,HostContext.hostProcessName())).sorted(Comparator.comparingInt((ProviderInfo info)->info.initOrder).reversed()).toList();}
 static boolean owns(ComponentInfo info){return info.applicationInfo.uid==android.os.Process.myUid()&&Objects.equals(info.processName==null?info.packageName:info.processName,HostContext.hostProcessName());}
 /** The launcher supplies read-only APKs and metadata; peer data paths grant no filesystem access. */
 public static synchronized void inspectInstalled(String encoded)throws Exception{
  persistentInstallations.addAll(readInstalled(encoded));
 }
 /** Authorization rows belong to the same authenticated activation as the
  * process directory. They are not permanent Android installations. */
 public static synchronized Set<String> enrollInstalled(HostBinderRpc.Peer peer,String encoded)throws Exception{
  Objects.requireNonNull(peer);Set<String> packages=readInstalled(encoded);
  if(encoded!=null){var records=new org.json.JSONArray(encoded);for(int i=0;i<records.length();i++){var value=records.getJSONObject(i);if(peer.packageName().equals(value.getString("packageName"))&&peer.uid()==value.getInt("uid")&&value.has("androidId"))peerAndroidIds.put(peer,value.getString("androidId"));}}
  Set<String> previous=peerInstallations.put(peer,packages);
  if(previous!=null)retireUnused(previous);
  Set<String> roots=new HashSet<>();for(String pkg:packages){Installation value=catalog.get(pkg);if(value!=null&&value.preparedRoot!=null)roots.add(value.preparedRoot);}
  return Set.copyOf(roots);
 }
 static synchronized void retirePeer(HostBinderRpc.Peer peer){
  peerAndroidIds.remove(peer);
  Set<String> packages=peerInstallations.remove(peer);if(packages!=null)retireUnused(packages);
 }
 private static void retireUnused(Set<String> packages){
  var next=new LinkedHashMap<>(catalog);var removedPackages=new LinkedHashMap<String,Installation>();
  for(String pkg:packages)if(!Objects.equals(pkg,name)&&!persistentInstallations.contains(pkg)&&peerInstallations.values().stream().noneMatch(values->values.contains(pkg))){
   var removed=next.remove(pkg);if(removed!=null)removedPackages.put(pkg,removed);
  }
  catalog=Collections.unmodifiableMap(next);
  for(var entry:removedPackages.entrySet()){
   HostDexFiles.uninstalled(entry.getValue().sourcePaths());
   for(var observer:List.copyOf(packageObservers))observer.onPackageRemoved(entry.getKey(),entry.getValue().parsedPackage.getUid());
  }
 }
 private static Set<String> readInstalled(String encoded)throws Exception{
  if(encoded==null)return Set.of();org.json.JSONArray records=new org.json.JSONArray(encoded);LinkedHashMap<String,Installation> next=new LinkedHashMap<>(catalog);Set<String> packages=new HashSet<>();
  for(int i=0;i<records.length();i++){
   var record=records.getJSONObject(i);String pkg=record.getString("packageName");packages.add(pkg);if(pkg.equals(name))continue;
   var files=record.getJSONArray("sourceApks");List<Path> sources=new ArrayList<>();for(int j=0;j<files.length();j++)sources.add(Path.of(files.getString(j)).toRealPath());
   ParsedPackage parsed=parse(sources);if(!pkg.equals(parsed.getPackageName()))throw new IllegalArgumentException("Installed APK package differs from its catalog identity");
   Path data=Path.of(record.getString("dataRoot")).normalize();if(!data.isAbsolute())throw new IllegalArgumentException("Installed package data path must be absolute");
   Installation installation=new Installation(parsed,data,record.getString("nativeRoot"),record.getInt("uid"),record.getLong("firstInstallTime"),record.getLong("lastUpdateTime"));
   String androidId=record.optString("androidId","");if(!androidId.isEmpty()){if(!androidId.matches("[0-9a-f]{16}"))throw new SecurityException("Invalid Android application identity");installation.androidId=androidId;}
   String prepared=record.optString("preparedRoot","");if(!prepared.isEmpty())installation.preparedRoot=Path.of(prepared).toRealPath().toString();
   installation.signingDetails();Installation previous=next.get(pkg);
   if(previous!=null){
    if(previous.parsedPackage.getUid()!=installation.parsedPackage.getUid()||!previous.verifiedSigningDetails().signaturesMatchExactly(installation.verifiedSigningDetails()))throw new SecurityException("Installed package identity changed during process enrollment");
    // Another Android bottle may use the same APK. Retain the existing package's
    // data owner; an authenticated peer contributes code and identity only.
    if(previous.androidId==null)previous.androidId=installation.androidId;
   }else next.put(pkg,installation);
   String converted=record.optString("convertedJar","");
   if(previous==null&&!converted.isEmpty())HostDexFiles.installed(Path.of(converted),sources);
  }
  var previous=catalog;catalog=Collections.unmodifiableMap(next);
  for(var entry:next.entrySet())if(!previous.containsKey(entry.getKey()))for(var observer:List.copyOf(packageObservers))observer.onPackageAdded(entry.getKey(),entry.getValue().parsedPackage.getUid());
  return Set.copyOf(packages);
 }
 /** The launcher removes peer metadata only after its process lifetime ends. */
 public static synchronized void removeInstalled(String packageName){
  if(Objects.equals(packageName,name))throw new IllegalArgumentException("Cannot remove the process's own package");
  persistentInstallations.remove(packageName);
  var next=new LinkedHashMap<>(catalog);var removed=next.remove(packageName);catalog=Collections.unmodifiableMap(next);
  if(removed!=null){HostDexFiles.uninstalled(removed.sourcePaths());for(var observer:List.copyOf(packageObservers))observer.onPackageRemoved(packageName,removed.parsedPackage.getUid());}
 }
 public static ActivityInfo activityInfo(String className)throws PackageManager.NameNotFoundException{ActivityInfo value=current.component(current.activities.get(new ComponentName(name,className)),PackageManager.GET_META_DATA,UserHandle.myUserId());if(value==null)throw new PackageManager.NameNotFoundException(className);return value;}
 public static Set<String> activityActions(ComponentName component){
  Installation pkg=find(component,UserHandle.myUserId());if(pkg==null)return Set.of();var activity=pkg.activities.get(component);if(activity==null)return Set.of();
  Set<String> actions=new HashSet<>();for(var info:activity.getIntents()){var filter=info.getIntentFilter();for(int i=0;i<filter.countActions();i++)actions.add(filter.getAction(i));}return Set.copyOf(actions);
 }
 public static boolean declaredProcess(String process){Installation pkg=current;if(pkg==null)return false;return Objects.equals(process,pkg.parsedPackage.getProcessName())||java.util.stream.Stream.of(pkg.parsedPackage.getActivities(),pkg.parsedPackage.getReceivers(),pkg.parsedPackage.getServices(),pkg.parsedPackage.getProviders()).flatMap(Collection::stream).anyMatch(value->Objects.equals(process,value.getProcessName()));}
 public static boolean permitsService(String caller,ServiceInfo service)throws Exception{
  return permitsComponent(caller,service,service.permission);
 }
 public static boolean permitsReceiver(String caller,ActivityInfo receiver)throws Exception{
  return permitsComponent(caller,receiver,receiver.permission);
 }
 private static boolean permitsComponent(String caller,ComponentInfo service,String permission)throws Exception{
  Installation source=catalog.get(caller),target=catalog.get(service.packageName);if(source==null||target==null)return false;
  if(source.parsedPackage.getUid()==android.os.Process.SYSTEM_UID||source.parsedPackage.getUid()==android.os.Process.ROOT_UID)return true;
  if(caller.equals(service.packageName))return true;if(!service.exported)return false;
  if(permission==null)return true;
  return permissionGranted(source,permission,Context.DEVICE_ID_DEFAULT);
 }
 /** One installed-package grant calculation for services, providers and framework clients. */
 static int checkUidPermission(String permission,int uid,int device){
  try{for(Installation source:catalog.values())if(source.parsedPackage.getUid()==uid&&permissionGranted(source,permission,device))return PackageManager.PERMISSION_GRANTED;}
  catch(Exception failure){throw new IllegalStateException("Android permission identity could not be verified",failure);}
  return PackageManager.PERMISSION_DENIED;
 }
 private static boolean permissionGranted(Installation source,String permission,int device)throws Exception{
  if(source.parsedPackage.getUid()==android.os.Process.SYSTEM_UID||source.parsedPackage.getUid()==android.os.Process.ROOT_UID)return true;
  if(!source.parsedPackage.getRequestedPermissions().contains(permission))return false;
  if(source==current&&HostPermissions.checkLocal(permission,source.parsedPackage.getUid(),device)==PackageManager.PERMISSION_GRANTED)return true;
  if(HostPermissions.normalPermissions(source.parsedPackage).contains(permission))return true;
  for(Installation declaring:catalog.values())for(var definition:declaring.parsedPackage.getPermissions())if(permission.equals(definition.getName())){
   int protection=definition.getProtectionLevel()&PermissionInfo.PROTECTION_MASK_BASE;
   if(protection==PermissionInfo.PROTECTION_NORMAL)return true;
   if(protection==PermissionInfo.PROTECTION_SIGNATURE)return source.verifiedSigningDetails().hasCommonSignerWithCapability(declaring.verifiedSigningDetails(),SigningDetails.CertCapabilities.PERMISSION);
   return source.parsedPackage.getUid()!=android.os.Process.myUid()&&HostPermissions.checkRemote(permission,source.parsedPackage.getUid(),device)==PackageManager.PERMISSION_GRANTED;
  }
  return source.parsedPackage.getUid()!=android.os.Process.myUid()&&HostPermissions.checkRemote(permission,source.parsedPackage.getUid(),device)==PackageManager.PERMISSION_GRANTED;
 }
 /** PackageManagerInternal service boundaries over the same installed catalog as IPackageManager. */
 public static boolean hostHasSignatureCapability(Object receiver,int uid1,int uid2,int capability)throws Exception{
  if(receiver instanceof PackageManagerInternal service)return service.hasSignatureCapability(uid1,uid2,capability);
  SigningDetails first=signingDetailsForUid(uid1),second=signingDetailsForUid(uid2);
  // Preserve PackageManagerService.PackageManagerInternalImpl's direction and lineage rule.
  return first.checkCapability(second,capability)||second.hasAncestorOrSelf(first);
 }
 private static SigningDetails signingDetailsForUid(int uid)throws Exception{
  SigningDetails result=SigningDetails.UNKNOWN;
  for(Installation pkg:catalog.values())if(pkg.parsedPackage.getUid()==uid){
   SigningDetails next=pkg.verifiedSigningDetails();
   // Shared-UID members contribute their real signing lineage, including revoked capabilities.
   result=result==SigningDetails.UNKNOWN?next:result.mergeLineageWith(next,SigningDetails.CapabilityMergeRule.MERGE_RESTRICTED_CAPABILITY);
  }
  return result;
 }
 public static int hostGetPackageUid(Object receiver,String packageName,long flags,int user){
  if(receiver instanceof PackageManagerInternal service)return service.getPackageUid(packageName,flags,user);
  Installation pkg=find(packageName,user);
  if(pkg==null||hostFilterAppAccess(null,packageName,Binder.getCallingUid(),user)
    ||!com.android.server.pm.parsing.pkg.AndroidPackageUtils.isMatchForSystemOnly(pkg.packageState,flags))return -1;
  return pkg.state(user).isInstalled()||((flags&(PackageManager.MATCH_UNINSTALLED_PACKAGES|PackageManager.MATCH_ARCHIVED_PACKAGES))!=0
    &&com.android.server.pm.pkg.PackageStateUtils.isMatch(pkg.packageState,flags))?pkg.parsedPackage.getUid():-1;
 }
 public static ApplicationInfo hostGetApplicationInfo(Object receiver,String packageName,long flags,int callingUid,int user){
  if(receiver instanceof PackageManagerInternal service)return service.getApplicationInfo(packageName,flags,callingUid,user);
  Installation pkg=find(packageName,user);
  return pkg==null||hostFilterAppAccess(null,packageName,callingUid,user)?null:pkg.app(flags,user);
 }
 public static ComponentName hostGetHomeActivitiesAsUser(Object receiver,List<ResolveInfo> candidates,int user)throws Exception{
  if(receiver instanceof PackageManagerInternal service)return service.getHomeActivitiesAsUser(candidates,user);
  candidates.addAll(query(new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME),null,PackageManager.MATCH_DEFAULT_ONLY,user,1));
  // Desktop presentation has no selected Android HOME-role holder.
  return null;
 }
 public static ProviderInfo hostResolveContentProvider(Object receiver,String authority,long flags,int user,int caller){
  if(receiver instanceof PackageManagerInternal service)return service.resolveContentProvider(authority,flags,user,caller);
  try{HostPlatformPackages.resolve(authority);}catch(RuntimeException failure){throw failure;}catch(Exception failure){throw new IllegalStateException("Android platform provider could not be resolved",failure);}
  for(Installation pkg:catalog.values()){
   if(hostFilterAppAccess(null,pkg.name,caller,user))continue;
   ProviderInfo info=pkg.component(pkg.authorities.get(authority),flags,user);if(info!=null)return info;
  }return null;
 }
 /** Every enrolled package in the supervisor's installation catalog is already
  * queryable by its enrolled peers. URI grants do not install packages or widen
  * that catalog, and need no second AppsFilter visibility database. */
 public static void hostGrantImplicitAccess(Object receiver,int user,Intent intent,int recipientAppId,int visibleUid,boolean direct,boolean retainOnUpdate){
  if(receiver instanceof PackageManagerInternal service){service.grantImplicitAccess(user,intent,recipientAppId,visibleUid,direct,retainOnUpdate);return;}
  if(user<0||recipientAppId<0||visibleUid<0)throw new IllegalArgumentException("Invalid package visibility identity");
 }
 public static void hostGrantImplicitAccess(Object receiver,int user,Intent intent,int recipientAppId,int visibleUid,boolean direct){hostGrantImplicitAccess(receiver,user,intent,recipientAppId,visibleUid,direct,false);}
 public static boolean hostFilterAppAccess(Object receiver,String packageName,int callingUid,int user){
  if(receiver instanceof PackageManagerInternal service)return service.filterAppAccess(packageName,callingUid,user);
  Installation target=find(packageName,user);
  if(target==null||!target.state(user).isInstalled())return true;
  if(callingUid==android.os.Process.ROOT_UID||callingUid==android.os.Process.SYSTEM_UID)return false;
  if(!HostAccountAccess.packageVisible(packageName,callingUid))return true;
  if(UserHandle.getUserId(callingUid)!=user)return true;
  // The supervisor supplies only packages visible in this installation's consented catalog.
  // Unknown callers never acquire visibility merely by naming an installed package.
  for(Installation caller:catalog.values())if(caller.parsedPackage.getUid()==callingUid&&caller.state(user).isInstalled())return false;
  return true;
 }
 /** Internal policy readers use the same original package records as the Binder
  * package manager. Snapshotting the index does not create another state owner. */
 public static com.android.server.pm.pkg.PackageStateInternal hostGetPackageStateInternal(Object receiver,String packageName){
  if(receiver instanceof PackageManagerInternal service)return service.getPackageStateInternal(packageName);
  Installation pkg=catalog.get(packageName);return pkg==null?null:pkg.packageState;
 }
 /** Native alignment inspection feeds the original package-state policy;
  * manifest overrides remain authoritative for this installed application. */
 public static synchronized boolean hostPageSizeCompatibility(int alignmentFlags){
  Installation value=Objects.requireNonNull(current,"Android application package is not inspected");
  int flags=value.parsedPackage.getPageSizeAppCompatFlags();
  if(alignmentFlags!=ApplicationInfo.PAGE_SIZE_APP_COMPAT_FLAG_ERROR)flags|=alignmentFlags;
  value.packageState.setPageSizeAppCompatFlags(flags);
  return value.packageState.isPageSizeAppCompatEnabled();
 }
 public static android.util.ArrayMap<String,? extends com.android.server.pm.pkg.PackageStateInternal> hostGetPackageStates(Object receiver){
  if(receiver instanceof PackageManagerInternal service)return service.getPackageStates();
  var result=new android.util.ArrayMap<String,com.android.server.pm.pkg.PackageStateInternal>();
  for(var entry:catalog.entrySet())result.put(entry.getKey(),entry.getValue().packageState);
  return result;
 }
 public static com.android.server.pm.pkg.AndroidPackage hostGetAndroidPackage(Object receiver,String packageName){
  if(receiver instanceof PackageManagerInternal service)return service.getAndroidPackage(packageName);
  Installation pkg=catalog.get(packageName);return pkg==null?null:pkg.parsedPackage;
 }
 public static com.android.server.pm.pkg.AndroidPackage hostGetPackage(Object receiver,String packageName){
  if(receiver instanceof PackageManagerInternal service)return service.getPackage(packageName);
  return hostGetAndroidPackage(null,packageName);
 }
 public static boolean hostWasPackageEverLaunched(Object receiver,String packageName,int user){
  if(receiver instanceof PackageManagerInternal service)return service.wasPackageEverLaunched(packageName,user);
  var state=hostGetPackageStateInternal(null,packageName);
  if(state==null)throw new IllegalArgumentException("Unknown package: "+packageName);
  return !state.getUserStateOrDefault(user).isNotLaunched();
 }
 public static boolean hostIsPackageStopped(Object receiver,String packageName,int user)throws PackageManager.NameNotFoundException{
  if(receiver instanceof PackageManagerInternal service)return service.isPackageStopped(packageName,user);
  int caller=Binder.getCallingUid();
  if(user<0)throw new IllegalArgumentException("Invalid Android user: "+user);
  if(caller!=android.os.Process.ROOT_UID&&caller!=android.os.Process.SYSTEM_UID&&UserHandle.getUserId(caller)!=user
    &&HostPermissions.check(android.Manifest.permission.INTERACT_ACROSS_USERS_FULL,caller,Context.DEVICE_ID_DEFAULT)!=PackageManager.PERMISSION_GRANTED)
   throw new SecurityException("Cross-user package access requires INTERACT_ACROSS_USERS_FULL");
  var state=hostGetPackageStateInternal(null,packageName);
  if(state==null||hostFilterAppAccess(null,packageName,caller,user))throw new PackageManager.NameNotFoundException(packageName);
  return state.getUserStateOrDefault(user).isStopped();
 }
 public static boolean hostNativeCode(Path file){for(Installation value:catalog.values())if(file.startsWith(Path.of(value.parsedPackage.getNativeLibraryDir())))return true;return false;}
 public static synchronized void hostClearProviders(){catalog=Map.of();persistentInstallations.clear();peerInstallations.clear();peerAndroidIds.clear();current=null;context=null;HostPermissions.clear();HostDexFiles.clearInstalled();}
 private static final class Installation {
  final String name;final ParsedPackage parsedPackage;final PackageSetting packageState;final PackageProperty properties;final Path dataRoot;final long firstInstallTime,lastUpdateTime;
  final Map<ComponentName,ParsedProvider> providers;final Map<ComponentName,ParsedService> services;final Map<ComponentName,ParsedActivity> activities;final Map<String,ParsedProvider> authorities;private volatile IntentIndex intentIndex;private volatile SigningDetails verifiedIdentity;
  private String preparedRoot,androidId;private boolean systemComponent;
  Installation(ParsedPackage parsed,Path data,String nativeRoot,int uid,long first,long last){
   this(parsed,data,nativeRoot,uid,first,last,null);
  }
  Installation(ParsedPackage parsed,Path data,String nativeRoot,int uid,long first,long last,String selectedAbi){
   name=parsed.getPackageName();dataRoot=data;firstInstallTime=first;lastUpdateTime=last;
   String abi=selectedAbi;
   if(abi==null){abi=Path.of(nativeRoot).getFileName().toString();if(!Set.of("armeabi-v7a","armeabi").contains(abi))abi="arm64-v8a";}
   parsed.setUid(uid).setNativeLibraryDir(nativeRoot).setPrimaryCpuAbi(abi);
   // System installation is authenticated image/package-manager state, not an
   // application manifest's claim about its own privilege.
   if(platform(parsed))parsed.setSystem(true);
   HostSharedLibraries.prepare(parsed,((AndroidPackageHidden)parsed).isSystem());
   parsed.hideAsFinal();parsedPackage=parsed;ApplicationInfo info=((AndroidPackageHidden)parsed).toAppInfoWithoutState();
   packageState=new PackageSetting(name,null,Path.of(parsed.getPath()).toFile(),info.flags,info.privateFlags,java.util.UUID.randomUUID());packageState.setPkg(parsed).setAppId(UserHandle.getAppId(uid)).setPrimaryCpuAbi(abi).setLastUpdateTime(last).setLongVersionCode(parsed.getLongVersionCode()).setTargetSdkVersion(parsed.getTargetSdkVersion());packageState.getOrCreateUserState(UserHandle.myUserId()).setInstalled(true).setStopped(false).setNotLaunched(false).setFirstInstallTimeMillis(first);
   HostSharedLibraries.install(parsed,packageState);
   var pp=new LinkedHashMap<ComponentName,ParsedProvider>();var ss=new LinkedHashMap<ComponentName,ParsedService>();var aa=new LinkedHashMap<ComponentName,ParsedActivity>();var byAuthority=new LinkedHashMap<String,ParsedProvider>();
   for(var value:parsed.getProviders()){pp.put(key(value),value);if(value.getAuthority()!=null)for(String authority:value.getAuthority().split(";"))byAuthority.putIfAbsent(authority,value);}for(var value:parsed.getServices())ss.put(key(value),value);for(var value:parsed.getActivities())aa.put(key(value),value);
   providers=Map.copyOf(pp);services=Map.copyOf(ss);activities=Map.copyOf(aa);authorities=Map.copyOf(byAuthority);properties=new PackageProperty();HostUiAccess.call(HostUiAccess.method(PackageProperty.class,"addAllProperties",com.android.server.pm.pkg.AndroidPackage.class),properties,parsed);
  }
  boolean isCurrent(){return current==this;}
  private static boolean platform(ParsedPackage parsed){return parsed.getPackageName().equals("android")&&parsed.getUid()==android.os.Process.SYSTEM_UID&&Path.of(parsed.getBaseApkPath()).equals(FrameworkResources.frameworkPath());}
  boolean platform(){return platform(parsedPackage);}
  List<Path> sourcePaths(){var paths=new ArrayList<Path>();paths.add(Path.of(parsedPackage.getBaseApkPath()));if(parsedPackage.getSplitCodePaths()!=null)for(String path:parsedPackage.getSplitCodePaths())paths.add(Path.of(path));return List.copyOf(paths);}
  void bindPaths(ApplicationInfo info){if(isCurrent()){HostContext.bindHostPaths(info);return;}info.dataDir=dataRoot.toString();info.credentialProtectedDataDir=info.dataDir;info.deviceProtectedDataDir=dataRoot.resolve("device_protected").toString();}
  SigningDetails signingDetails()throws Exception{if(platform())return SigningDetails.UNKNOWN;packageInfo(PackageManager.GET_SIGNING_CERTIFICATES,UserHandle.myUserId());return parsedPackage.getSigningDetails();}
  synchronized SigningDetails verifiedSigningDetails()throws Exception{
   // The supplied framework archive is code owned by the runtime, not a signed
   // third-party APK. System authority never impersonates a publisher certificate.
   if(platform())return SigningDetails.UNKNOWN;
   if(verifiedIdentity!=null)return verifiedIdentity;
   SigningDetails identity=null;int minimum=android.util.apk.ApkSignatureVerifier.getMinimumSignatureSchemeVersionForTargetSdk(parsedPackage.getTargetSdkVersion());
   for(Path path:sourcePaths()){var result=android.util.apk.ApkSignatureVerifier.verify(android.content.pm.parsing.result.ParseTypeImpl.forDefaultParsing(),path.toString(),minimum);if(result.isError())throw new java.io.IOException(result.getErrorMessage(),result.getException());SigningDetails next=result.getResult();if(identity==null)identity=next;else if(!identity.signaturesMatchExactly(next))throw new SecurityException("Android package splits have different signing identities");}
   parsedPackage.setSigningDetails(identity);packageState.setSigningDetails(identity);return verifiedIdentity=identity;
  }
 private PackageUserStateInternal state(int user){return packageState.getUserStateOrDefault(user);}
 private long componentFlags(long flags){return (flags&(PackageManager.MATCH_DIRECT_BOOT_AWARE|PackageManager.MATCH_DIRECT_BOOT_UNAWARE))==0?flags|PackageManager.MATCH_DIRECT_BOOT_AWARE|PackageManager.MATCH_DIRECT_BOOT_UNAWARE:flags;}
  private ApplicationInfo app(long flags,int user){
  ApplicationInfo value=PackageInfoUtils.generateApplicationInfo(parsedPackage,flags,state(user),user,packageState);
  if(value!=null){bindPaths(value);if(systemComponent&&"system".equals(value.processName))value.processName="android.system";}return value;
 }
 private <T extends ComponentInfo>T component(ParsedMainComponent value,long flags,int user){
  flags=componentFlags(flags);if(value==null||user!=UserHandle.myUserId()||!PackageUserStateUtils.isMatch(state(user),false,parsedPackage.isEnabled(),value,flags))return null;
  ComponentInfo info;var application=app(flags,user);
  if(value instanceof ParsedActivity activity)info=PackageInfoUtils.generateActivityInfo(parsedPackage,activity,flags,state(user),application,user,packageState);
  else if(value instanceof ParsedProvider provider)info=PackageInfoUtils.generateProviderInfo(parsedPackage,provider,flags,state(user),application,user,packageState);
  else if(value instanceof ParsedService service)info=PackageInfoUtils.generateServiceInfo(parsedPackage,service,flags,state(user),application,user,packageState);
  else return null;
  if(systemComponent&&"system".equals(info.processName))info.processName="android.system";
  @SuppressWarnings("unchecked") T result=(T)info;return result;
 }
 private PackageInfo packageInfo(long flags,int user)throws Exception{
  ParsedPackage pkg=parsedPackage;if(pkg==null)return null;flags=componentFlags(flags);
  if(!platform()&&(flags&(PackageManager.GET_SIGNATURES|PackageManager.GET_SIGNING_CERTIFICATES))!=0&&pkg.getSigningDetails()==SigningDetails.UNKNOWN){synchronized(pkg){if(pkg.getSigningDetails()==SigningDetails.UNKNOWN){
   var result=android.util.apk.ApkSignatureVerifier.unsafeGetCertsWithoutVerification(android.content.pm.parsing.result.ParseTypeImpl.forDefaultParsing(),pkg.getBaseApkPath(),android.util.apk.ApkSignatureVerifier.getMinimumSignatureSchemeVersionForTargetSdk(pkg.getTargetSdkVersion()));
   if(result.isError())throw new java.io.IOException(result.getErrorMessage(),result.getException());pkg.setSigningDetails(result.getResult());packageState.setSigningDetails(result.getResult());
  }}}
  Set<String> grants=(flags&PackageManager.GET_PERMISSIONS)==0?Set.of():isCurrent()?HostPermissions.grantedPermissions():HostPermissions.normalPermissions(parsedPackage);
  PackageInfo info=PackageInfoUtils.generate(pkg,null,flags,firstInstallTime,lastUpdateTime,Set.of(),grants,state(user),user,packageState);
  if(info!=null){if(info.applicationInfo!=null)bindPaths(info.applicationInfo);for(ComponentInfo[] family:new ComponentInfo[][]{info.activities,info.receivers,info.services,info.providers})if(family!=null)for(ComponentInfo component:family)if(component.applicationInfo!=null)bindPaths(component.applicationInfo);}
  return info;
 }
 private int setting(ComponentName component){
  if(!providers.containsKey(component)&&!services.containsKey(component)&&!activities.containsKey(component)&&!intents().receiverInfo.containsKey(component))throw new IllegalArgumentException("Unknown application component: "+component);
  var state=state(UserHandle.myUserId());return state.isComponentEnabled(component.getClassName())?PackageManager.COMPONENT_ENABLED_STATE_ENABLED:state.isComponentDisabled(component.getClassName())?PackageManager.COMPONENT_ENABLED_STATE_DISABLED:PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
 }
 private record IntentEntry(ParsedMainComponent info,IntentFilter filter){}
 /** Upstream indexes and matching algorithms, with only local installation/state policy. */
 private final class Resolver extends com.android.server.IntentResolver<IntentEntry,ResolveInfo>{
  @Override protected IntentFilter getIntentFilter(IntentEntry entry){return entry.filter;}
  @Override protected boolean isPackageForFilter(String own,IntentEntry entry){return own.equals(entry.info.getPackageName());}
  @Override protected IntentEntry[] newArray(int size){return new IntentEntry[size];}
  @Override protected boolean allowFilterResult(IntentEntry entry,List<ResolveInfo> found){for(var value:found){ComponentInfo info=value.serviceInfo!=null?value.serviceInfo:value.providerInfo!=null?value.providerInfo:value.activityInfo;if(info.name.equals(entry.info.getName())&&info.packageName.equals(entry.info.getPackageName()))return false;}return true;}
  @Override protected ResolveInfo newResult(com.android.server.pm.Computer unused,IntentEntry entry,int match,int user,long flags){return resolution(entry.info,entry.filter,match,flags,user);}
  @Override protected void sortResults(List<ResolveInfo> values){values.sort(com.android.server.pm.resolution.ComponentResolver.RESOLVE_PRIORITY_SORTER);}
  List<ResolveInfo> query(Intent intent,String type,long flags,int user){return queryIntent(null,intent,type,(flags&PackageManager.MATCH_DEFAULT_ONLY)!=0,user,flags);}
 }
 private record IntentIndex(Resolver services,Resolver activities,Resolver receivers,Resolver providers,Map<ComponentName,ParsedActivity> receiverInfo){}
 private ResolveInfo resolution(ParsedMainComponent source,IntentFilter filter,int match,long flags,int user){
  ComponentInfo info=component(source,flags,user);if(info==null)return null;var result=new ResolveInfo();
  if(info instanceof ServiceInfo service)result.serviceInfo=service;else if(info instanceof ProviderInfo provider)result.providerInfo=provider;else result.activityInfo=(ActivityInfo)info;
  if(filter!=null){result.match=match;result.priority=filter.getPriority();result.isDefault=filter.hasCategory(Intent.CATEGORY_DEFAULT);if((flags&PackageManager.GET_RESOLVED_FILTER)!=0)result.filter=new IntentFilter(filter);}
  result.labelRes=info.labelRes;result.nonLocalizedLabel=info.nonLocalizedLabel;result.icon=info.icon;result.resolvePackageName=info.packageName;return result;
 }
 private synchronized IntentIndex intents(){
  if(intentIndex!=null)return intentIndex;
  Resolver sr=new Resolver(),ar=new Resolver(),rr=new Resolver(),pr=new Resolver();Map<ComponentName,ParsedActivity> receivers=new LinkedHashMap<>();var parsed=parsedPackage;
  if(parsed!=null){
   for(var value:parsed.getServices())for(var filter:value.getIntents())sr.addFilter(null,new IntentEntry(value,filter.getIntentFilter()));
   for(var value:parsed.getActivities())for(var filter:value.getIntents())ar.addFilter(null,new IntentEntry(value,filter.getIntentFilter()));
   for(var value:parsed.getProviders())for(var filter:value.getIntents())pr.addFilter(null,new IntentEntry(value,filter.getIntentFilter()));
   for(var value:parsed.getReceivers()){receivers.put(key(value),value);for(var filter:value.getIntents())rr.addFilter(null,new IntentEntry(value,filter.getIntentFilter()));}
  }
  return intentIndex=new IntentIndex(sr,ar,rr,pr,Map.copyOf(receivers));
 }
 private List<ResolveInfo> query(Intent requested,String type,long flags,int user,int kind)throws Exception{
  Intent intent=requested.getComponent()==null&&requested.getSelector()!=null?requested.getSelector():requested;
  if(user!=UserHandle.myUserId()||intent.getPackage()!=null&&!Objects.equals(name,intent.getPackage()))return List.of();
  ComponentName explicit=intent.getComponent();if(explicit!=null){if(!Objects.equals(name,explicit.getPackageName()))return List.of();ParsedMainComponent info=kind==0?services.get(explicit):kind==1?activities.get(explicit):kind==2?intents().receiverInfo.get(explicit):providers.get(explicit);ResolveInfo found=resolution(info,null,0,flags,user);return found==null?List.of():List.of(found);}
  IntentIndex index=intents();return (kind==0?index.services:kind==1?index.activities:kind==2?index.receivers:index.providers).query(intent,type,flags,user);
 }
 private void setEnabled(ComponentName component,int value,int flags){
  setting(component);if(value<0||value>4)throw new IllegalArgumentException("Invalid component enabled state");
  
  String method=value==0?"restoreComponentLPw":value==1?"enableComponentLPw":"disableComponentLPw";
  HostUiAccess.call(HostUiAccess.method(PackageSetting.class,method,String.class,int.class),packageState,component.getClassName(),UserHandle.myUserId());
  monitors.notifyPackageChanged(name,(flags&PackageManager.DONT_KILL_APP)!=0,new ArrayList<>(List.of(component.getClassName())),parsedPackage.getUid(),null,new int[]{UserHandle.myUserId()},new int[0],null,new Handler(Looper.getMainLooper()));
  ParsedService info=services.get(component);if(isCurrent()&&info!=null&&(value==2||value==3||value==0&&!info.isEnabled()))HostJobScheduler.hostCancelService(component);
 }
 }
 private static List<ResolveInfo> query(Intent intent,String type,long flags,int user,int kind)throws Exception{List<ResolveInfo> result=new ArrayList<>();for(Installation pkg:catalog.values())result.addAll(pkg.query(intent,type,flags,user,kind));result.sort(com.android.server.pm.resolution.ComponentResolver.RESOLVE_PRIORITY_SORTER);return result;}
 private static ComponentInfo component(ComponentName name,long flags,int user,int kind){Installation pkg=find(name,user);if(pkg==null)return null;return pkg.component(kind==0?pkg.activities.get(name):kind==1?pkg.intents().receiverInfo.get(name):kind==2?pkg.providers.get(name):pkg.services.get(name),flags,user);}
 private static String[] packagesForUid(int uid){String[] packages=catalog.values().stream().filter(pkg->pkg.parsedPackage.getUid()==uid).map(pkg->pkg.name).toArray(String[]::new);return packages.length==0?null:packages;}
 private static PackageInfo describe(Installation pkg,long flags,int user)throws Exception{
  if(pkg==null)return null;PackageInfo value=pkg.packageInfo(flags,user);
  return (flags&(PackageManager.GET_SIGNATURES|PackageManager.GET_SIGNING_CERTIFICATES))!=0&&HostGoogleProviders.applies(pkg.name)?HostGoogleProviders.describe(context,value,pkg.parsedPackage.getMetaData(),pkg.verifiedSigningDetails()):value;
 }
 private static Object invoke(Object proxy,Method method,Object[] arguments)throws Throwable{
  Object[] a=arguments==null?new Object[0]:arguments;String op=method.getName();
  if(method.getDeclaringClass()==Object.class)return switch(op){case "toString"->"Installed Android package service";case "hashCode"->System.identityHashCode(proxy);case "equals"->proxy==a[0];default->throw new AssertionError(op);};
  return switch(op){
   case "asBinder" -> binder;
   case "registerPackageMonitorCallback" -> {monitors.registerPackageMonitorCallback((IRemoteCallback)a[0],(int)a[1],Binder.getCallingUid());yield null;}
   case "unregisterPackageMonitorCallback" -> {monitors.unregisterPackageMonitorCallback((IRemoteCallback)a[0]);yield null;}
   case "getApplicationInfo" -> {Installation pkg=find((String)a[0],(int)a[2]);yield pkg==null?null:pkg.app((long)a[1],(int)a[2]);}
   case "notifyPackageUse" -> {Installation pkg=find((String)a[0],UserHandle.getUserId(Binder.getCallingUid()));if(pkg!=null)synchronized(pkg.packageState){pkg.packageState.getTransientState().setLastPackageUsageTimeInMills((int)a[1],System.currentTimeMillis());}yield null;}
   case "getPackageInfo" -> describe(find((String)a[0],(int)a[2]),(long)a[1],(int)a[2]);
   case "getPackageInfoVersioned" -> {VersionedPackage p=(VersionedPackage)a[0];Installation pkg=find(p.getPackageName(),(int)a[2]);yield pkg!=null&&(p.getLongVersionCode()==PackageManager.VERSION_CODE_HIGHEST||p.getLongVersionCode()==pkg.parsedPackage.getLongVersionCode())?describe(pkg,(long)a[1],(int)a[2]):null;}
   case "getActivityInfo","getReceiverInfo","getProviderInfo","getServiceInfo" -> component((ComponentName)a[0],(long)a[1],(int)a[2],op.equals("getActivityInfo")?0:op.equals("getReceiverInfo")?1:op.equals("getProviderInfo")?2:3);
   case "queryIntentServices","queryIntentActivities","queryIntentReceivers","queryIntentContentProviders" -> new ParceledListSlice<>(query((Intent)a[0],(String)a[1],(long)a[2],(int)a[3],op.equals("queryIntentServices")?0:op.equals("queryIntentActivities")?1:op.equals("queryIntentReceivers")?2:3));
   case "resolveService","resolveIntent" -> {List<ResolveInfo> matches=query((Intent)a[0],(String)a[1],(long)a[2],(int)a[3],op.equals("resolveService")?0:1);yield matches.isEmpty()?null:matches.get(0);}
   case "resolveContentProvider" -> {HostPlatformPackages.resolve((String)a[0]);ProviderInfo result=null;for(var pkg:catalog.values()){result=pkg.component(pkg.authorities.get(a[0]),(long)a[1],(int)a[2]);if(result!=null)break;}yield result;}
   case "getInstalledPackages" -> {List<PackageInfo> result=new ArrayList<>();if((int)a[1]==UserHandle.myUserId())for(var pkg:catalog.values())result.add(describe(pkg,(long)a[0],(int)a[1]));yield new ParceledListSlice<>(result);}
   case "getInstalledApplications" -> {List<ApplicationInfo> result=new ArrayList<>();if((int)a[1]==UserHandle.myUserId())for(var pkg:catalog.values())result.add(pkg.app((long)a[0],(int)a[1]));yield new ParceledListSlice<>(result);}
   case "isPackageAvailable" -> find((String)a[0],(int)a[1])!=null;
   case "getPackageUid" -> {Installation pkg=find((String)a[0],(int)a[2]);yield pkg==null?-1:pkg.parsedPackage.getUid();}
   case "getPackagesForUid" -> packagesForUid((int)a[0]);
   case "getNameForUid" -> {String[] packages=packagesForUid((int)a[0]);yield packages==null?null:packages[0];}
   case "getNamesForUids" -> {int[] uids=(int[])a[0];if(uids==null)yield null;String[] names=new String[uids.length];for(int i=0;i<uids.length;i++){String[] packages=packagesForUid(uids[i]);names[i]=packages==null?null:packages[0];}yield names;}
   case "getAllPackages" -> List.copyOf(catalog.keySet());
   case "getTargetSdkVersion" -> {Installation pkg=catalog.get(a[0]);yield pkg==null?-1:pkg.parsedPackage.getTargetSdkVersion();}
   case "getInstallerPackageName" -> {if(!catalog.containsKey(a[0]))throw new IllegalArgumentException("Unknown package: "+a[0]);yield null;}
   case "getInstallSourceInfo" -> find((String)a[0],(int)a[1])==null?null:new InstallSourceInfo(null,null,null,null);
   case "checkSignatures" -> {Installation left=find((String)a[0],(int)a[2]),right=find((String)a[1],(int)a[2]);yield left==null||right==null?PackageManager.SIGNATURE_UNKNOWN_PACKAGE:com.android.server.pm.PackageManagerServiceUtils.compareSignatures(left.signingDetails(),right.signingDetails());}
   case "hasSigningCertificate" -> {Installation pkg=find((String)a[0],UserHandle.getCallingUserId());yield pkg!=null&&((int)a[2]==PackageManager.CERT_INPUT_RAW_X509?pkg.signingDetails().hasCertificate((byte[])a[1]):(int)a[2]==PackageManager.CERT_INPUT_SHA256&&pkg.signingDetails().hasSha256Certificate((byte[])a[1]));}
   case "isInstantApp" -> false;
   case "isDeviceUpgrading" -> false;
   case "getInstantAppCookie","getInstantAppAndroidId","getSetupWizardPackageName" -> null;
   case "hasSystemFeature" -> HostActivityManager.hostHasSystemFeature((String)a[0],(int)a[1]);
   case "getSystemAvailableFeatures" -> new ParceledListSlice<>(HostActivityManager.hostHardwareFeatures());
   case "getSystemSharedLibraryNames" -> HostSharedLibraries.names();
   case "getSharedLibraries" -> new ParceledListSlice<>(HostSharedLibraries.libraries());
   case "getPermissionControllerPackageName","getSdkSandboxPackageName" -> null;
   case "getPropertyAsUser" -> {Installation pkg=find((String)a[1],(int)a[3]);yield pkg==null?null:pkg.properties.getProperty((String)a[0],(String)a[1],(String)a[2]);}
   case "queryProperty" -> {List<PackageManager.Property> result=new ArrayList<>();for(var pkg:catalog.values()){var found=pkg.properties.queryProperty((String)a[0],(int)a[1],packageName->!Objects.equals(pkg.name,packageName));if(found!=null)result.addAll(found);}yield new ParceledListSlice<>(result);}
   case "getComponentEnabledSetting" -> {Installation pkg=find((ComponentName)a[0],(int)a[1]);if(pkg==null)throw new IllegalArgumentException("Unknown package");yield pkg.setting((ComponentName)a[0]);}
   case "setComponentEnabledSetting" -> {Installation pkg=find((ComponentName)a[0],(int)a[3]);var peer=HostBinderRpc.callingPeer();if(pkg!=current||peer!=null&&!pkg.name.equals(peer.packageName()))throw new SecurityException("Another package owns this component");pkg.setEnabled((ComponentName)a[0],(int)a[1],(int)a[2]);yield null;}
   case "getApplicationEnabledSetting" -> {Installation pkg=find((String)a[0],(int)a[1]);if(pkg==null)throw new IllegalArgumentException("Unknown package");yield pkg.state((int)a[1]).getEnabledState();}
   case "checkPermission" -> {Installation pkg=find((String)a[1],(int)a[2]);yield pkg==null?PackageManager.PERMISSION_DENIED:HostPermissions.check((String)a[0],pkg.parsedPackage.getUid(),Context.DEVICE_ID_DEFAULT);}
   case "currentToCanonicalPackageNames","canonicalToCurrentPackageNames" -> ((String[])a[0]).clone();
   default -> throw new RemoteException("Android package operation is unavailable: "+op);
  };
 }
}
