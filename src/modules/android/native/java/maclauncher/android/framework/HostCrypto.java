package maclauncher.android.framework;

import java.security.Security;
import com.android.internal.org.bouncycastle.jce.provider.BouncyCastleProvider;
import com.android.org.conscrypt.OpenSSLProvider;
import com.android.org.conscrypt.JSSEProvider;

/** Adds the supplied Android Java algorithms alongside the JVM's native providers.
 * Algorithm preferences come from the launch-time java.security supplement,
 * before the JDK constructs its provider list; registration retains its order. */
public final class HostCrypto {
    private HostCrypto(){}
    public static synchronized void initialize(){
        if(Security.getProvider(BouncyCastleProvider.PROVIDER_NAME)==null)
            Security.addProvider(new BouncyCastleProvider());
        if(Security.getProvider("AndroidOpenSSL")==null){
            // The JDK's URLConnection verifier is only a fallback after
            // SunJSSE's endpoint check. Use Conscrypt's original OpenJDK
            // strict verifier without changing the JDK HTTPS verifier.
            com.android.org.conscrypt.Conscrypt.setDefaultHostnameVerifier(com.android.org.conscrypt.OkHostnameVerifier.strictInstance());
            Security.addProvider(new OpenSSLProvider());
        }
        if(Security.getProvider("HarmonyJSSE")==null)Security.addProvider(new JSSEProvider());
    }
}
