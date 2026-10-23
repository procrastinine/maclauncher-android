import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;
import maclauncher.build.PendingIntentCalls;

/** Exact process-service boundaries; the upstream class and its state remain intact. */
final class ServicePortability {
    static byte[] transform(String name,byte[] original){
        original=ShortcutPortability.transform(name,original);
        if(name.equals("com/android/server/usb/UsbUserPermissionManager.class"))return usbPermissionSnapshot(original);
        // Statsd is an Android system collector. AOSP supplies its own host
        // boundary methods; retain the complete API and event/buffer lifecycle.
        if(name.equals("android/util/StatsLog.class"))return upstreamStatsHost(original);
        if(name.equals("android/webkit/WebViewFactory.class"))return browserProvider(original);
        if(name.startsWith("android/app/SystemServiceRegistry$"))return devicePolicyClient(original);
        boolean registry=name.equals("android/os/ServiceManager.class"),observers=name.equals("com/android/server/content/ContentService$ObserverCollector.class");
        // Bind the platform interface wherever upstream invokes it, including
        // nested service/session classes. Call-site owners are not an API allowlist.
        boolean packages=new String(original,java.nio.charset.StandardCharsets.ISO_8859_1).contains("android/content/pm/PackageManagerInternal");
        boolean activity=new String(original,java.nio.charset.StandardCharsets.ISO_8859_1).contains("android/app/ActivityManagerInternal");
        boolean packageConfiguration=new String(original,java.nio.charset.StandardCharsets.ISO_8859_1).contains("com/android/server/wm/ActivityTaskManagerInternal");
        boolean users=new String(original,java.nio.charset.StandardCharsets.ISO_8859_1).contains("com/android/server/pm/UserManagerInternal");
        boolean appOps=new String(original,java.nio.charset.StandardCharsets.ISO_8859_1).contains("android/app/AppOpsManagerInternal");
        String caller=name.substring(0,name.length()-6);
        boolean pending=PendingIntentCalls.applies(caller);
        if(!registry&&!observers&&!packages&&!activity&&!packageConfiguration&&!users&&!appOps&&!pending&&!name.equals("com/android/server/accounts/AccountManagerService$Session.class"))return original;
        ClassNode node=new ClassNode();new ClassReader(original).accept(node,0);
        for(MethodNode method:node.methods){
            if(name.equals("com/android/server/accounts/AccountManagerService$Session.class"))accountSession(method);
            if(pending)for(AbstractInsnNode instruction:method.instructions){
                if(!(instruction instanceof MethodInsnNode call)||(call.getOpcode()!=Opcodes.INVOKEVIRTUAL&&call.getOpcode()!=Opcodes.INVOKEINTERFACE))continue;
                if(!PendingIntentCalls.binds(caller,method.name,call.owner,call.name))continue;
                call.setOpcode(Opcodes.INVOKESTATIC);call.owner=PendingIntentCalls.TARGET;
                call.desc=PendingIntentCalls.descriptor(call.desc);call.itf=false;
            }
            if(packages)for(AbstractInsnNode instruction:method.instructions){
                if(!(instruction instanceof MethodInsnNode call)||!call.owner.equals("android/content/pm/PackageManagerInternal"))continue;
                String target=null;
                if(call.name.equals("hasSignatureCapability")&&call.desc.equals("(III)Z"))target="hostHasSignatureCapability";
                if(call.name.equals("getPackageUid")&&call.desc.equals("(Ljava/lang/String;JI)I"))target="hostGetPackageUid";
                if(call.name.equals("filterAppAccess")&&call.desc.equals("(Ljava/lang/String;II)Z"))target="hostFilterAppAccess";
                if(call.name.equals("getApplicationInfo")&&call.desc.equals("(Ljava/lang/String;JII)Landroid/content/pm/ApplicationInfo;"))target="hostGetApplicationInfo";
                if(call.name.equals("resolveContentProvider")&&call.desc.equals("(Ljava/lang/String;JII)Landroid/content/pm/ProviderInfo;"))target="hostResolveContentProvider";
                if(call.name.equals("grantImplicitAccess")&&(call.desc.equals("(ILandroid/content/Intent;IIZZ)V")||call.desc.equals("(ILandroid/content/Intent;IIZ)V")))target="hostGrantImplicitAccess";
                if(call.name.equals("getPackageStateInternal")&&call.desc.equals("(Ljava/lang/String;)Lcom/android/server/pm/pkg/PackageStateInternal;"))target="hostGetPackageStateInternal";
                if(call.name.equals("getPackageStates")&&call.desc.equals("()Landroid/util/ArrayMap;"))target="hostGetPackageStates";
                if(call.name.equals("getAndroidPackage")&&call.desc.equals("(Ljava/lang/String;)Lcom/android/server/pm/pkg/AndroidPackage;"))target="hostGetAndroidPackage";
                if(call.name.equals("getPackage")&&call.desc.equals("(Ljava/lang/String;)Lcom/android/server/pm/pkg/AndroidPackage;"))target="hostGetPackage";
                if(call.name.equals("wasPackageEverLaunched")&&call.desc.equals("(Ljava/lang/String;I)Z"))target="hostWasPackageEverLaunched";
                if(call.name.equals("isPackageStopped")&&call.desc.equals("(Ljava/lang/String;I)Z"))target="hostIsPackageStopped";
                if(call.name.equals("getHomeActivitiesAsUser")&&call.desc.equals("(Ljava/util/List;I)Landroid/content/ComponentName;"))target="hostGetHomeActivitiesAsUser";
                if(target==null)continue;
                call.setOpcode(Opcodes.INVOKESTATIC);call.owner="maclauncher/android/framework/HostPackages";call.name=target;
                call.desc="(Ljava/lang/Object;"+call.desc.substring(1);call.itf=false;
            }

            if(activity)for(AbstractInsnNode instruction:method.instructions){
                if(!(instruction instanceof MethodInsnNode call)||!call.owner.equals("android/app/ActivityManagerInternal"))continue;
                String target=null;
                if(call.name.equals("handleIncomingUser")&&call.desc.equals("(IIIZILjava/lang/String;Ljava/lang/String;)I"))target="hostHandleIncomingUser";
                if(call.name.equals("enforceCallingPermission")&&call.desc.equals("(Ljava/lang/String;Ljava/lang/String;)V"))target="hostEnforceCallingPermission";
                if(call.name.equals("checkContentProviderUriPermission")&&call.desc.equals("(Landroid/net/Uri;III)I"))target="hostCheckContentProviderUriPermission";
                if(call.name.equals("isAppForeground")&&call.desc.equals("(I)Z"))target="hostIsAppForeground";
                if(call.name.equals("getUidCapability")&&call.desc.equals("(I)I"))target="hostUidCapability";
                if(target==null)continue;
                call.setOpcode(Opcodes.INVOKESTATIC);call.owner="maclauncher/android/framework/HostActivityManager";call.name=target;
                call.desc="(Ljava/lang/Object;"+call.desc.substring(1);call.itf=false;
            }

            if(packageConfiguration)for(AbstractInsnNode instruction:method.instructions){
                if(!(instruction instanceof MethodInsnNode call)||!call.owner.equals("com/android/server/wm/ActivityTaskManagerInternal"))continue;
                String target=call.name.equals("getApplicationConfig")&&call.desc.equals("(Ljava/lang/String;I)Lcom/android/server/wm/ActivityTaskManagerInternal$PackageConfig;")?"get":call.name.equals("createPackageConfigurationUpdater")&&call.desc.equals("(Ljava/lang/String;I)Lcom/android/server/wm/ActivityTaskManagerInternal$PackageConfigurationUpdater;")?"create":null;
                if(target==null)continue;
                call.setOpcode(Opcodes.INVOKESTATIC);call.owner="com/android/server/wm/HostPackageConfiguration";call.name=target;
                call.desc="(Ljava/lang/Object;"+call.desc.substring(1);call.itf=false;
            }
            if(users)for(AbstractInsnNode instruction:method.instructions){
                if(!(instruction instanceof MethodInsnNode call)||!call.owner.equals("com/android/server/pm/UserManagerInternal"))continue;
                boolean host=switch(call.name+call.desc){
                    case "addUserRestrictionsListener(Lcom/android/server/pm/UserManagerInternal$UserRestrictionsListener;)V",
                         "removeUserRestrictionsListener(Lcom/android/server/pm/UserManagerInternal$UserRestrictionsListener;)V",
                         "getProfileParentId(I)I","getUserRestriction(ILjava/lang/String;)Z","getUserIds()[I]",
                         "isUserUnlockingOrUnlocked(I)Z","getUserProperties(I)Landroid/content/pm/UserProperties;"->true;
                    default->false;
                };
                if(!host)continue;
                call.setOpcode(Opcodes.INVOKESTATIC);call.owner="maclauncher/android/framework/HostUserService";
                call.name="host"+Character.toUpperCase(call.name.charAt(0))+call.name.substring(1);
                call.desc="(Ljava/lang/Object;"+call.desc.substring(1);call.itf=false;
            }
            if(appOps)for(AbstractInsnNode instruction:method.instructions){
                if(!(instruction instanceof MethodInsnNode call)||!call.owner.equals("android/app/AppOpsManagerInternal")||!call.name.equals("setGlobalRestriction")||!call.desc.equals("(IZLandroid/os/IBinder;)V"))continue;
                call.setOpcode(Opcodes.INVOKESTATIC);call.owner="maclauncher/android/framework/HostAppOps";call.name="hostSetGlobalRestriction";
                call.desc="(Ljava/lang/Object;IZLandroid/os/IBinder;)V";call.itf=false;
            }

            if(observers)for(AbstractInsnNode instruction:method.instructions){
                if(instruction instanceof MethodInsnNode call&&call.owner.equals("android/app/ActivityManagerInternal")&&call.name.equals("getUidProcessState")&&call.desc.equals("(I)I")){
                    call.setOpcode(Opcodes.INVOKESTATIC);call.owner="maclauncher/android/framework/HostActivityManager";call.name="hostUidProcessState";call.desc="(Landroid/app/ActivityManagerInternal;I)I";call.itf=false;
                }
            }
            boolean bridge=registry&&method.name.equals("getIServiceManager")&&method.desc.equals("()Landroid/os/IServiceManager;");
            if(!bridge)continue;
            method.instructions.clear();method.tryCatchBlocks.clear();method.localVariables=null;
            method.visibleLocalVariableAnnotations=null;method.invisibleLocalVariableAnnotations=null;
            method.access&=~(Opcodes.ACC_NATIVE|Opcodes.ACC_ABSTRACT);
            int slot=0;for(Type argument:Type.getArgumentTypes(method.desc)){
                method.instructions.add(new VarInsnNode(argument.getOpcode(Opcodes.ILOAD),slot));slot+=argument.getSize();
            }
            method.instructions.add(new MethodInsnNode(Opcodes.INVOKESTATIC,"maclauncher/android/framework/HostServices","client",method.desc,false));
            method.instructions.add(new InsnNode(Type.getReturnType(method.desc).getOpcode(Opcodes.IRETURN)));
        }
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_MAXS);node.accept(writer);return writer.toByteArray();
    }

    /** The supplied AOSP accessory serializer indexes the device-grant snapshot.
     * Keep its complete permission implementation and XML format; select the
     * matching accessory snapshot at that one array read. */
    private static byte[] usbPermissionSnapshot(byte[] original){
        ClassNode node=new ClassNode();new ClassReader(original).accept(node,0);int corrected=0;
        for(MethodNode method:node.methods){
            if(!method.name.startsWith("lambda$scheduleWritePermissionsLocked$"))continue;
            Integer device=null,accessory=null;
            if(method.localVariables!=null)for(LocalVariableNode local:method.localVariables){
                if(local.name.equals("grantedValuesForDevices")&&local.desc.equals("[[Z"))device=local.index;
                if(local.name.equals("grantedValuesForAccessories")&&local.desc.equals("[[Z"))accessory=local.index;
            }
            if(device==null||accessory==null)throw new IllegalStateException("USB permission snapshot locals changed");
            for(AbstractInsnNode instruction:method.instructions){
                if(!(instruction instanceof MethodInsnNode write)||!write.owner.equals("android/hardware/usb/AccessoryFilter")||!write.name.equals("write"))continue;
                AbstractInsnNode cursor=write.getPrevious();
                while(cursor!=null&&!(cursor instanceof MethodInsnNode call&&call.owner.equals("java/lang/Boolean")&&call.name.equals("toString")&&call.desc.equals("(Z)Ljava/lang/String;")))cursor=cursor.getPrevious();
                if(cursor==null)throw new IllegalStateException("USB accessory permission serializer changed");
                cursor=previousCode(cursor);if(cursor.getOpcode()!=Opcodes.BALOAD)throw new IllegalStateException("USB accessory grant value changed");
                cursor=previousCode(previousCode(previousCode(previousCode(cursor))));
                if(!(cursor instanceof VarInsnNode load)||load.getOpcode()!=Opcodes.ALOAD||(load.var!=device&&load.var!=accessory))throw new IllegalStateException("USB accessory grant array changed");
                load.var=accessory;corrected++;
            }
        }
        if(corrected!=1)throw new IllegalStateException("USB accessory permission snapshot was not found exactly once");
        ClassWriter writer=new ClassWriter(0);node.accept(writer);return writer.toByteArray();
    }
    private static AbstractInsnNode previousCode(AbstractInsnNode instruction){
        do{instruction=instruction.getPrevious();}while(instruction!=null&&instruction.getOpcode()<0);
        if(instruction==null)throw new IllegalStateException("Android instruction boundary changed");return instruction;
    }

    private static void accountSession(MethodNode method){
        if(method.name.equals("<init>")||method.name.equals("close")&&method.desc.equals("()V")){
            String hook=method.name.equals("<init>")?"createdSession":"retiredSession";
            for(AbstractInsnNode instruction:method.instructions.toArray())if(instruction.getOpcode()==Opcodes.RETURN){
                InsnList call=new InsnList();call.add(new VarInsnNode(Opcodes.ALOAD,0));call.add(new MethodInsnNode(Opcodes.INVOKESTATIC,"maclauncher/android/framework/HostAccountAccess",hook,"(Ljava/lang/Object;)V",false));method.instructions.insertBefore(instruction,call);
            }
        }
        if(method.name.equals("bind")&&method.desc.equals("()V")){
            InsnList check=new InsnList();LabelNode proceed=new LabelNode();check.add(new VarInsnNode(Opcodes.ALOAD,0));check.add(new MethodInsnNode(Opcodes.INVOKESTATIC,"maclauncher/android/framework/HostAccountAccess","bindSession","(Ljava/lang/Object;)Z",false));check.add(new JumpInsnNode(Opcodes.IFNE,proceed));check.add(new InsnNode(Opcodes.RETURN));check.add(proceed);check.add(new FrameNode(Opcodes.F_SAME,0,null,0,null));method.instructions.insert(check);
        }
    }

    /** macOS has no Android device administrator. The complete original client
     * already defines absent-service behavior; its factory must still return it.
     * Select the declared return type, not an unstable anonymous-class number. */
    private static byte[] devicePolicyClient(byte[] original){
        ClassNode node=new ClassNode();new ClassReader(original).accept(node,0);boolean changed=false;
        for(MethodNode method:node.methods){
            if(!method.name.equals("createService")||!Type.getReturnType(method.desc).getDescriptor().equals("Landroid/app/admin/DevicePolicyManager;"))continue;
            for(AbstractInsnNode instruction:method.instructions)if(instruction instanceof MethodInsnNode call&&
                call.getOpcode()==Opcodes.INVOKESTATIC&&call.owner.equals("android/os/ServiceManager")&&call.name.equals("getServiceOrThrow")&&call.desc.equals("(Ljava/lang/String;)Landroid/os/IBinder;")){
                call.name="getService";changed=true;
            }
        }
        if(!changed)return original;ClassWriter writer=new ClassWriter(0);node.accept(writer);return writer.toByteArray();
    }

    private static byte[] browserProvider(byte[] original){
        ClassNode node=new ClassNode();new ClassReader(original).accept(node,0);
        for(MethodNode method:node.methods){
            if(!method.name.equals("getProviderClass")||!method.desc.equals("()Ljava/lang/Class;"))continue;
            method.instructions.clear();method.tryCatchBlocks.clear();method.localVariables=null;
            method.visibleLocalVariableAnnotations=null;method.invisibleLocalVariableAnnotations=null;
            method.instructions.add(new MethodInsnNode(Opcodes.INVOKESTATIC,"maclauncher/android/web/WebKitFactory","providerClass",method.desc,false));
            method.instructions.add(new InsnNode(Opcodes.ARETURN));
        }
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_MAXS);node.accept(writer);return writer.toByteArray();
    }

    private static byte[] upstreamStatsHost(byte[] original){
        ClassNode node=new ClassNode();new ClassReader(original).accept(node,0);
        var supplied=new java.util.HashMap<String,MethodNode>();
        String suffix="$ravenwood";
        for(MethodNode method:node.methods)if(method.name.endsWith(suffix))
            supplied.put(method.name.substring(0,method.name.length()-suffix.length())+method.desc,method);
        for(MethodNode method:node.methods){
            MethodNode host=supplied.get(method.name+method.desc);
            if(host==null)continue;
            boolean isStatic=(method.access&Opcodes.ACC_STATIC)!=0;
            if(isStatic!=((host.access&Opcodes.ACC_STATIC)!=0)||(host.access&(Opcodes.ACC_NATIVE|Opcodes.ACC_ABSTRACT))!=0)
                throw new IllegalStateException("AOSP statistics host boundary has an incompatible signature");
            method.instructions.clear();method.tryCatchBlocks.clear();method.localVariables=null;
            method.visibleLocalVariableAnnotations=null;method.invisibleLocalVariableAnnotations=null;
            method.access&=~(Opcodes.ACC_NATIVE|Opcodes.ACC_ABSTRACT);
            int slot=0;
            if(!isStatic){method.instructions.add(new VarInsnNode(Opcodes.ALOAD,0));slot=1;}
            for(Type argument:Type.getArgumentTypes(method.desc)){
                method.instructions.add(new VarInsnNode(argument.getOpcode(Opcodes.ILOAD),slot));slot+=argument.getSize();
            }
            method.instructions.add(new MethodInsnNode(isStatic?Opcodes.INVOKESTATIC:Opcodes.INVOKESPECIAL,node.name,host.name,host.desc,false));
            method.instructions.add(new InsnNode(Type.getReturnType(method.desc).getOpcode(Opcodes.IRETURN)));
        }
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_MAXS);node.accept(writer);return writer.toByteArray();
    }
}


/** Local system dependencies of the complete original shortcut implementation. */
final class ShortcutPortability {
    static byte[] transform(String name,byte[] original){
        if(!name.startsWith("com/android/server/pm/Shortcut")||!name.endsWith(".class"))return original;
        ClassNode node=new ClassNode();new ClassReader(original).accept(node,0);
        for(MethodNode method:node.methods)for(AbstractInsnNode instruction:method.instructions.toArray()){
            if(!(instruction instanceof MethodInsnNode call))continue;
            // These local services are bound at their actual call sites below.
            // Keep the original typed fields, including real LocalServices when present.
            if(method.name.equals("<init>")&&call.owner.equals("java/util/Objects")&&call.name.equals("requireNonNull")&&call.desc.equals("(Ljava/lang/Object;)Ljava/lang/Object;")){
                AbstractInsnNode next=call.getNext();while(next!=null&&next.getOpcode()<0)next=next.getNext();
                if(next instanceof TypeInsnNode cast&&java.util.Set.of("android/content/pm/PackageManagerInternal","com/android/server/pm/UserManagerInternal","android/app/usage/UsageStatsManagerInternal","android/app/ActivityManagerInternal","android/app/role/RoleManager").contains(cast.desc))method.instructions.remove(call);
            }
            if(call.owner.equals("android/app/ActivityManagerInternal")&&call.name.equals("getUidProcessState")&&call.desc.equals("(I)I")){
                call.setOpcode(Opcodes.INVOKESTATIC);call.owner="maclauncher/android/framework/HostActivityManager";call.name="hostUidProcessState";call.desc="(Landroid/app/ActivityManagerInternal;I)I";call.itf=false;
            }
            if(call.owner.equals("android/app/usage/UsageStatsManagerInternal")&&call.name.equals("reportShortcutUsage")&&call.desc.equals("(Ljava/lang/String;Ljava/lang/String;I)V")){
                call.setOpcode(Opcodes.INVOKESTATIC);call.owner="com/android/server/usage/HostUsageEvents";call.name="reportShortcutUsage";call.desc="(Landroid/app/usage/UsageStatsManagerInternal;Ljava/lang/String;Ljava/lang/String;I)V";call.itf=false;
            }
            if(call.owner.equals("android/os/SELinux")&&call.name.equals("restorecon")&&call.desc.equals("(Ljava/io/File;)Z")){
                call.owner="maclauncher/android/framework/HostFilesystem";call.name="restoreSecurityContext";
            }
        }
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_MAXS);node.accept(writer);return writer.toByteArray();
    }
}
