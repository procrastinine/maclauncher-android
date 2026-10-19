package maclauncher.android.framework;

import android.os.Parcel;

/** Original motion-event serialization with the process's native Binder Parcel owner. */
public final class HostMotionEvent {
    private HostMotionEvent(){}
    public static native long nativeReadFromParcel(long destination,Parcel parcel);
    public static native void nativeWriteToParcel(long pointer,Parcel parcel);
}
