package maclauncher.android.framework;

import android.content.Context;
import android.content.pm.UserInfo;
import android.os.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;

/** Current private application's user/storage identity, behind the full AOSP user client. */
public final class HostUserService {
 private HostUserService(){}
 private static Path files;
 private static int user;
 private static boolean registered;
 private static final Binder binder=new Binder();
 private static final Set<com.android.server.pm.UserManagerInternal.UserRestrictionsListener> restrictionListeners=Collections.newSetFromMap(new IdentityHashMap<>());
 private static final RemoteCallbackList<IUserRestrictionsListener> userRestrictionCallbacks=new RemoteCallbackList<>();
 private static final IUserManager service=(IUserManager)Proxy.newProxyInstance(IUserManager.class.getClassLoader(),new Class<?>[]{IUserManager.class},HostUserService::invoke);
 static{binder.attachInterface(service,"android.os.IUserManager");}
 public static synchronized void install(Context context){
  Path path=context.getFilesDir().toPath().toAbsolutePath().normalize();
  if(!Files.isDirectory(path)||!Files.isReadable(path)||!Files.isWritable(path))throw new IllegalStateException("Application storage is unavailable");
  if(files==null){files=path;user=context.getUserId();}else if(user!=context.getUserId())throw new SecurityException("Other Android users are unavailable");
  if(!registered){HostServices.register(Context.USER_SERVICE,service,"android.os.IUserManager");registered=true;}
 }
 private static boolean own(int id){return id==user;}
 private static UserInfo info(){UserInfo info=new UserInfo(user,"Android",null,UserInfo.FLAG_FULL);info.userType=UserManager.USER_TYPE_FULL_SECONDARY;info.serialNumber=user;return info;}
 private static boolean unlocked(int id){return own(id)&&files!=null&&Files.isDirectory(files)&&Files.isReadable(files)&&Files.isWritable(files);}
 public static void hostAddUserRestrictionsListener(Object receiver,com.android.server.pm.UserManagerInternal.UserRestrictionsListener listener){
  if(receiver instanceof com.android.server.pm.UserManagerInternal original){original.addUserRestrictionsListener(listener);return;}
  synchronized(HostUserService.class){restrictionListeners.add(Objects.requireNonNull(listener));}
 }
 public static void hostRemoveUserRestrictionsListener(Object receiver,com.android.server.pm.UserManagerInternal.UserRestrictionsListener listener){
  if(receiver instanceof com.android.server.pm.UserManagerInternal original){original.removeUserRestrictionsListener(listener);return;}
  synchronized(HostUserService.class){restrictionListeners.remove(listener);}
 }
 public static int hostGetProfileParentId(Object receiver,int id){
  if(receiver instanceof com.android.server.pm.UserManagerInternal original)return original.getProfileParentId(id);
  return own(id)?user:UserHandle.USER_NULL;
 }
 public static boolean hostGetUserRestriction(Object receiver,int id,String key){
  if(receiver instanceof com.android.server.pm.UserManagerInternal original)return original.getUserRestriction(id,key);
  try{return service.getUserRestrictions(id).getBoolean(Objects.requireNonNull(key));}catch(RemoteException failure){throw failure.rethrowFromSystemServer();}
 }
 public static int[] hostGetUserIds(Object receiver){
  if(receiver instanceof com.android.server.pm.UserManagerInternal original)return original.getUserIds();
  return new int[]{user};
 }
 public static boolean hostIsUserUnlockingOrUnlocked(Object receiver,int id){
  if(receiver instanceof com.android.server.pm.UserManagerInternal original)return original.isUserUnlockingOrUnlocked(id);
  return unlocked(id);
 }
 public static android.content.pm.UserProperties hostGetUserProperties(Object receiver,int id){
  if(receiver instanceof com.android.server.pm.UserManagerInternal original)return original.getUserProperties(id);
  if(!own(id))throw new IllegalArgumentException("Unknown Android user");
  return new android.content.pm.UserProperties.Builder().build();
 }
 private static Object invoke(Object proxy,Method method,Object[] arguments)throws Throwable{
  Object[] a=arguments==null?new Object[0]:arguments;String operation=method.getName();
  if(method.getDeclaringClass()==Object.class)return switch(operation){case "toString"->"Local Android user service";case "hashCode"->System.identityHashCode(proxy);case "equals"->proxy==a[0];default->throw new AssertionError(operation);};
  return switch(operation){
   case "asBinder" -> binder;
   case "addUserRestrictionsListener" -> {userRestrictionCallbacks.register((IUserRestrictionsListener)a[0]);yield null;}
   case "isUserUnlocked","isUserUnlockingOrUnlocked","isUserRunning" -> unlocked((int)a[0]);
   case "getUserInfo" -> own((int)a[0])?info():null;
   case "getUsers" -> List.of(info());
   case "getProfiles" -> own((int)a[0])?List.of(info()):List.of();
   case "getProfileIds","getProfileIdsExcludingHidden" -> own((int)a[0])?new int[]{user}:new int[0];
   case "getProfileParent" -> null;
   case "getCredentialOwnerProfile","getProfileParentId" -> own((int)a[0])?user:UserHandle.USER_NULL;
   case "getMainUserId","getBootUser" -> user;
   case "getCommunalProfileId","getPreviousFullUserToEnterForeground" -> UserHandle.USER_NULL;
   case "getUserSerialNumber","getUserHandle" -> own((int)a[0])?user:-1;
   case "isSameProfileGroup" -> own((int)a[0])&&own((int)a[1]);
   case "isUserOfType" -> own((int)a[0])&&UserManager.USER_TYPE_FULL_SECONDARY.equals(a[1]);
   case "getProfileType" -> own((int)a[0])?(info().isProfile()?info().userType:""):null;
   case "getUserRestrictions" -> {if(!own((int)a[0]))throw new SecurityException("Other Android users are unavailable");yield new Bundle();}
   case "getApplicationRestrictions","getApplicationRestrictionsForUser" -> {if(!Objects.equals(HostContext.hostApplicationInfoSnapshot().packageName,a[0])||a.length>1&&!own((int)a[1]))throw new SecurityException("Other application restrictions are unavailable");yield new Bundle();}
   case "getDefaultGuestRestrictions" -> new Bundle();
   case "getUserRestrictionSources","getGuestUsers" -> List.of();
   case "getUserRestrictionSource","getRemainingCreatableUserCount","getRemainingCreatableProfileCount" -> 0;
   case "isAdminUser","isDemoUser","isPreCreated","isRestricted","isQuietModeEnabled","isHeadlessSystemUserMode","hasBaseUserRestriction","hasUserRestriction","hasUserRestrictionOnAnyUser","isSettingRestrictedForUser","canAddMoreUsersOfType","canAddMoreProfilesToUser","canAddMoreManagedProfiles","canHaveRestrictedProfile","canAddPrivateProfile","isUserSwitcherEnabled","someUserHasSeedAccount","someUserHasAccount" -> false;
   case "getUserAccount","getSeedAccountName","getSeedAccountType","getSeedAccountOptions","getUserIcon" -> null;
   default -> throw new RemoteException("Android user operation is unavailable: "+operation);
  };
 }
}
