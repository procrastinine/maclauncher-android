package maclauncher.android.framework;

import android.app.AppOpsManager;
import android.content.Context;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.os.IPermissionController;

/** Native Android services query the same package, grant and app-operation owners. */
final class HostPermissionController extends IPermissionController.Stub implements AutoCloseable {
    private final Context context;
    HostPermissionController(Context context) throws android.os.RemoteException {
        this.context=context;
        HostServices.manager().addService("permission",this,false,0);
    }
    @Override public boolean checkPermission(String permission,int pid,int uid) {
        return HostPermissions.check(permission,uid,Context.DEVICE_ID_DEFAULT)==PackageManager.PERMISSION_GRANTED;
    }
    @Override public int noteOp(String op,int uid,String packageName) {
        return context.getSystemService(AppOpsManager.class).noteOpNoThrow(op,uid,packageName);
    }
    @Override public String[] getPackagesForUid(int uid) {
        return context.getPackageManager().getPackagesForUid(uid);
    }
    @Override public boolean isRuntimePermission(String permission) {
        try {
            return (context.getPackageManager().getPermissionInfo(permission,0).protectionLevel
                    &PermissionInfo.PROTECTION_MASK_BASE)==PermissionInfo.PROTECTION_DANGEROUS;
        } catch(PackageManager.NameNotFoundException absent) {return false;}
    }
    @Override public int getPackageUid(String packageName,int flags) {
        try {return context.getPackageManager().getPackageUid(packageName,flags);}
        catch(PackageManager.NameNotFoundException absent) {return -1;}
    }
    @Override public void close() {
        HostServices.unregister("permission",this,"android.os.IPermissionController");
    }
}
