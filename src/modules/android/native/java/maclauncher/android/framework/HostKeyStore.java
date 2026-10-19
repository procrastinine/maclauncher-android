package maclauncher.android.framework;

import android.os.IServiceManager;
import java.nio.file.Path;
import java.util.*;

/** Process bootstrap only; original keystore2 and KeyMint own every key and operation. */
public final class HostKeyStore {
    private HostKeyStore() {}
    private static final class Declarations {
        static final Map<String,String> VALUES=readDeclarations();
    }
    /** Original native publisher declarations, available without starting KeyStore. */
    public static Map<String,String> declarations(){return Declarations.VALUES;}
    private static Map<String,String> readDeclarations(){
        Properties properties=new Properties();
        try(var source=HostKeyStore.class.getResourceAsStream("keystore-services.properties")){
            if(source==null)throw new IllegalStateException("Android KeyStore service declarations are unavailable");
            properties.load(source);
        }catch(java.io.IOException failure){throw new IllegalStateException("Android KeyStore service declarations could not be read",failure);}
        Map<String,String> result=new TreeMap<>();
        for(String name:properties.stringPropertyNames()){
            String descriptor=properties.getProperty(name);
            if(name.isBlank()||descriptor.isBlank())throw new IllegalStateException("Android KeyStore service declaration is invalid");
            result.put(name,descriptor);
        }
        if(result.isEmpty())throw new IllegalStateException("Android KeyStore service declarations are unavailable");
        return Collections.unmodifiableMap(result);
    }
    public static void start(Path runtime,Path database) {
        System.load(runtime.resolve("native/libandroid-keystore.dylib").toString());
        HostServices.enablePlatformAuthorization();
        startNative(HostServices.manager(),database.toString());
    }
    private static native void startNative(IServiceManager manager,String database);
}
