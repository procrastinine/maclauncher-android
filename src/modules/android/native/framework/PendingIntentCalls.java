package maclauncher.build;

/** Shared classfile/DEX binding of original PendingIntent service destinations.
 * The original controller and record retain token state and delivery policy. */
public final class PendingIntentCalls {
    private PendingIntentCalls(){}
    public static final String TARGET="maclauncher/android/framework/HostPendingIntentDispatch";
    public static boolean applies(String caller){return caller.equals("com/android/server/am/PendingIntentController")||caller.equals("com/android/server/am/PendingIntentRecord")||caller.equals("com/android/server/am/ActivityManagerConstants");}
    public static boolean binds(String caller,String method,String owner,String name){
        if(!applies(caller))return false;
        return switch(owner){
            case "android/app/ActivityManagerInternal"->java.util.Set.of("getUidProcessState","enforceBroadcastOptionsPermissions","tempAllowlistForPendingIntent","broadcastIntentInPackage","startServiceInPackage").contains(name);
            case "com/android/server/wm/ActivityTaskManagerInternal"->java.util.Set.of("startActivitiesInPackage","startActivityInPackage","sendActivityResult","isUidForeground","clearPendingResultForActivity").contains(name);
            case "com/android/server/AlarmManagerInternal"->name.equals("remove");
            case "com/android/server/am/UserController"->name.equals("getCurrentOrTargetUserId");
            case "com/android/server/am/ActivityManagerService"->name.equals("scheduleUpdateBinderHeavyHitterWatcherConfig")&&method.equals("<init>");
            default->false;
        };
    }
    public static String descriptor(String original){return "(Ljava/lang/Object;"+original.substring(1).replace("Lcom/android/server/wm/Task;","Ljava/lang/Object;");}
}
