package maclauncher.android.framework;

import android.os.Parcel;

/** Platform objects retained by the original native Parcel's Binder owner. */
public final class HostObjectParcel {
    private HostObjectParcel(){}
    public static native void writeNativeHandle(Parcel parcel,long handle);
    public static native long readNativeHandle(Parcel parcel,Class<?> expected);
}
