package io.github.kksimp.dex2jvm;

/** Owned opt-in Android VM boundaries. Host JVM classes are not input DEX. */
final class NativeLoadRepoint {
    private static final String TARGET = target();
    private static String target() {
        String name = System.getProperty("maclauncher.dex.nativeLoadTarget");
        if (name == null) return null;
        name = name.replace('.', '/');
        if (!name.matches("[A-Za-z_$][A-Za-z0-9_$]*(/[A-Za-z_$][A-Za-z0-9_$]*)*"))
            throw new IllegalArgumentException("Invalid guest native-load redirect class");
        return name;
    }
    static GuestRepoint.Target lookup(boolean isStatic, boolean isVirtual, String owner, String name, String desc) {
        if (TARGET == null) return null;
        if (isStatic && owner.equals("java/lang/ClassLoader") && name.equals("getSystemClassLoader") &&
                desc.equals("()Ljava/lang/ClassLoader;"))
            return new GuestRepoint.Target("maclauncher/android/framework/HostDexFiles", name, desc);
        if (isVirtual && owner.equals("java/lang/Runtime") && desc.equals("(Ljava/lang/String;)V")) {
            if (name.equals("load")) return new GuestRepoint.Target(TARGET, "runtimeLoad", "(Ljava/lang/Runtime;Ljava/lang/String;)V");
            if (name.equals("loadLibrary")) return new GuestRepoint.Target(TARGET, "runtimeLoadLibrary", "(Ljava/lang/Runtime;Ljava/lang/String;)V");
        }
        if (!isStatic || !owner.equals("java/lang/System")) return null;
        if ((name.equals("load") || name.equals("loadLibrary")) && desc.equals("(Ljava/lang/String;)V"))
            return new GuestRepoint.Target(TARGET, name, desc);
        if (name.equals("mapLibraryName") && desc.equals("(Ljava/lang/String;)Ljava/lang/String;"))
            return new GuestRepoint.Target(TARGET, name, desc);
        if (name.equals("getProperty") && (desc.equals("(Ljava/lang/String;)Ljava/lang/String;") ||
                desc.equals("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")))
            return new GuestRepoint.Target(TARGET, name, desc);
        return null;
    }
}
