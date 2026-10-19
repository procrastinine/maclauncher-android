package maclauncher.android.framework;

import java.security.spec.ECParameterSpec;
import java.util.Objects;
import java.util.WeakHashMap;

/** Android's optional curve-name field attached to the JDK-owned parameters. */
public final class HostEcParameters {
    private HostEcParameters(){}
    private static final WeakHashMap<ECParameterSpec,String> names=new WeakHashMap<>();
    public static synchronized String getCurveName(ECParameterSpec parameters){return names.get(Objects.requireNonNull(parameters));}
    public static synchronized void setCurveName(ECParameterSpec parameters,String name){Objects.requireNonNull(parameters);if(name==null)names.remove(parameters);else names.put(parameters,name);}
}
