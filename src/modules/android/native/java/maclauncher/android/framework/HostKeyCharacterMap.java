package maclauncher.android.framework;

import android.os.Parcel;

/** Supplied keyboard-map serialization with the process's original Binder Parcel owner. */
public final class HostKeyCharacterMap {
    private HostKeyCharacterMap(){}
    public static native long nativeReadFromParcel(Parcel parcel);
    public static native void nativeWriteToParcel(long pointer,Parcel parcel);
}
