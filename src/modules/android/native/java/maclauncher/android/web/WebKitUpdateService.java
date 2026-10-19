package maclauncher.android.web;

import android.content.pm.PackageInfo;
import android.os.RemoteException;
import android.webkit.*;
import maclauncher.android.framework.HostServices;

/** Installed browser discovery for the original WebView update-service clients. */
final class WebKitUpdateService extends IWebViewUpdateService.Stub {
    private static final String PACKAGE="maclauncher.android.webkit";
    static void install(){
        try{HostServices.manager().addService("webviewupdate",new WebKitUpdateService(),false,0);}
        catch(RemoteException failure){throw failure.rethrowFromSystemServer();}
    }
    private static WebViewProviderInfo provider(){return new WebViewProviderInfo(PACKAGE,"MacLauncher WebKit",true,false,null);}
    @Override public PackageInfo getCurrentWebViewPackage(){return WebKitFactory.packageInfo();}
    @Override public String getCurrentWebViewPackageName(){return PACKAGE;}
    @Override public WebViewProviderInfo getDefaultWebViewPackage(){return provider();}
    @Override public WebViewProviderInfo[] getValidWebViewPackages(){return new WebViewProviderInfo[]{provider()};}
    @Override public WebViewProviderInfo[] getAllWebViewPackages(){return getValidWebViewPackages();}
    @Override public WebViewProviderResponse waitForAndGetProvider(){return new WebViewProviderResponse(getCurrentWebViewPackage(),WebViewProviderResponse.STATUS_SUCCESS);}
    @Override public String changeProviderAndSetting(String requested){
        if(!PACKAGE.equals(requested))WebKitFactory.unavailable("requested browser provider: "+requested);
        return PACKAGE;
    }
    // WebKit's system frameworks require no Android ELF relocation file.
    @Override public void notifyRelroCreationCompleted(){}
}
