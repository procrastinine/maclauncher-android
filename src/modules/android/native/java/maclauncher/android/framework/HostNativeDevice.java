package maclauncher.android.framework;

/** Native platform queries borrow the bound Android process and original APIs. */
public final class HostNativeDevice {
    private HostNativeDevice(){}
    private static android.content.pm.PackageInfo packageInfo(){
        var context=HostApplication.applicationContext();
        try{return context.getPackageManager().getPackageInfo(HostContext.hostPackageName(),0);}
        catch(android.content.pm.PackageManager.NameNotFoundException failure){throw new IllegalStateException("The bound Android package is unavailable",failure);}
    }
    public static String string(String field){
        return switch(field){case "package"->HostContext.hostPackageName();case "versionName"->packageInfo().versionName;default->null;};
    }
    public static long version(){return packageInfo().getLongVersionCode();}
    public static String[] locale(){var value=java.util.Locale.getDefault();return new String[]{value.getLanguage(),value.getCountry()};}
    public static int lowMemory(){
        var thread=android.app.ActivityThread.currentActivityThread();if(thread==null)return 0;
        // The original ActivityThread owns delivery to Activities, Services,
        // ContentProviders and the Application on its existing main handler.
        android.app.IApplicationThread process=thread.getApplicationThread();
        try{process.scheduleLowMemory();return 1;}
        catch(android.os.RemoteException failure){throw new IllegalStateException("Android memory notification could not be delivered",failure);}
    }
}
