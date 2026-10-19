package maclauncher.android;

import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.Provider;

/** Runtime-owned content identities, independent of later APK provider changes. */
public final class HostDigests {
    private static final Provider PROVIDER=initialProvider();
    private HostDigests(){}

    /** Retain the VM's original provider before an application changes providers. */
    static void initialize(){sha256();}

    private static Provider initialProvider(){
        try{return MessageDigest.getInstance("SHA-256").getProvider();}
        catch(NoSuchAlgorithmException failure){throw new AssertionError("VM SHA-256 implementation is unavailable",failure);}
    }

    public static MessageDigest sha256(){
        try{return MessageDigest.getInstance("SHA-256",PROVIDER);}
        catch(NoSuchAlgorithmException failure){throw new AssertionError("VM SHA-256 implementation is unavailable",failure);}
    }
}
