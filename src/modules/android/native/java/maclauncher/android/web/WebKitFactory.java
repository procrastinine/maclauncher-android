package maclauncher.android.web;

import android.app.Activity;
import android.content.*;
import android.content.pm.*;
import android.net.Uri;
import android.os.*;
import android.webkit.*;
import java.nio.file.*;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;

/** Acquisition of the native browser beneath the original Android provider lifecycle. */
public final class WebKitFactory implements WebViewFactoryProvider {
    private static Path directory;
    private static Path providerSource;
    private static boolean networkAllowed;
    private static volatile String version;
    private static final Set<String> diagnostics=ConcurrentHashMap.newKeySet();
    private final WebKitCookies cookies=new WebKitCookies();
    private final WebKitStorage storage=new WebKitStorage();
    private final Map<Context,WebViewDatabase> databases=new WeakHashMap<>();
    public static synchronized void configure(Path privateDirectory,boolean allowNetwork,Path source){
        Objects.requireNonNull(privateDirectory);Path next=privateDirectory.toAbsolutePath().normalize();
        Path origin=Objects.requireNonNull(source).toAbsolutePath().normalize();
        if(directory!=null&&(!directory.equals(next)||networkAllowed!=allowNetwork||!origin.equals(providerSource)))throw new IllegalStateException("WebKit already belongs to another application");
        boolean first=directory==null;directory=next;providerSource=origin;networkAllowed=allowNetwork;initialize();
        if(first)WebKitUpdateService.install();
    }
    public static boolean available(){return directory!=null;}
    private static synchronized void initialize(){
        if(version!=null)return;if(directory==null)throw new IllegalStateException("WebKit application storage is unavailable");
        try{Files.createDirectories(directory);}catch(java.io.IOException e){throw new IllegalStateException("Cannot open browser storage",e);}
        version=WebKitNative.initialize(directory.toString(),directory.getParent().getParent().toString(),networkAllowed);
    }
    public static String userAgent(){initialize();return "Mozilla/5.0 (Linux; Android) AppleWebKit/"+version+" (KHTML, like Gecko) MacLauncher/1.0";}
    static PackageInfo packageInfo(){
        initialize();PackageInfo info=new PackageInfo();info.packageName="maclauncher.android.webkit";info.versionName=version;
        info.setLongVersionCode(Arrays.stream(version.split("\\.")).limit(3).reduce(0L,(n,s)->n*1000+Long.parseLong(s.replaceAll("[^0-9]","").isEmpty()?"0":s.replaceAll("[^0-9]","")),(a,b)->a+b));
        info.applicationInfo=new ApplicationInfo();info.applicationInfo.packageName=info.packageName;info.applicationInfo.name="MacLauncher WebKit";info.applicationInfo.uid=android.os.Process.myUid();info.applicationInfo.dataDir=directory.toString();
        info.applicationInfo.enabled=true;
        info.applicationInfo.sourceDir=providerSource.toString();
        info.applicationInfo.publicSourceDir=info.applicationInfo.sourceDir;
        return info;
    }
    public static Class<?> providerClass(){
        PackageInfo info=packageInfo();
        try{
            Field field=WebViewFactory.class.getDeclaredField("sPackageInfo");field.setAccessible(true);field.set(null,info);
        }catch(ReflectiveOperationException e){throw new IllegalStateException("Cannot publish browser provider identity",e);}
        return WebKitFactory.class;
    }
    public static WebViewFactoryProvider create(WebViewDelegate delegate){initialize();return new WebKitFactory();}
    static void unavailable(String operation){if(diagnostics.add(operation))System.err.println("[android-webkit] Browser capability is unavailable: "+operation);}
    static void complete(Runnable callback){if(callback==null)return;Looper loop=Looper.myLooper();if(loop==null)loop=Looper.getMainLooper();if(loop==null)callback.run();else new Handler(loop).post(callback);}
    @Override public WebViewProvider createWebView(WebView view,WebView.PrivateAccess access){return WebKitView.create(view,access);}
    @Override public CookieManager getCookieManager(){return cookies;}
    @Override public WebStorage getWebStorage(){return storage;}
    @Override public ClassLoader getWebViewClassLoader(){return WebKitFactory.class.getClassLoader();}
    @Override public GeolocationPermissions getGeolocationPermissions(){return locations;}
    private final GeolocationPermissions locations=new GeolocationPermissions(){
        private final Set<String> allowed=ConcurrentHashMap.newKeySet();
        @Override public void getOrigins(ValueCallback<Set<String>> cb){complete(()->cb.onReceiveValue(Set.copyOf(allowed)));}
        @Override public void getAllowed(String origin,ValueCallback<Boolean> cb){complete(()->cb.onReceiveValue(allowed.contains(origin)));}
        @Override public void allow(String origin){allowed.add(Objects.requireNonNull(origin));}
        @Override public void clear(String origin){allowed.remove(origin);}
        @Override public void clearAll(){allowed.clear();}
    };
    @Override public TokenBindingService getTokenBindingService(){unavailable("TLS token binding");return null;}
    @Override public TracingController getTracingController(){return new TracingController(){public void start(TracingConfig config){unavailable("browser tracing");}public boolean stop(java.io.OutputStream out,java.util.concurrent.Executor executor){return false;}public boolean isTracing(){return false;}};}
    @Override public ServiceWorkerController getServiceWorkerController(){return workers;}
    private final ServiceWorkerController workers=new ServiceWorkerController(){
        private final ServiceWorkerWebSettings settings=new ServiceWorkerWebSettings(){int cache=WebSettings.LOAD_DEFAULT;boolean content=true,file=false,block=!networkAllowed;
            public void setCacheMode(int value){cache=value;unavailable("service worker cache override");}public int getCacheMode(){return cache;}
            public void setAllowContentAccess(boolean value){content=value;}public boolean getAllowContentAccess(){return content;}
            public void setAllowFileAccess(boolean value){file=value;}public boolean getAllowFileAccess(){return file;}
            public void setBlockNetworkLoads(boolean value){block=value;unavailable("service worker network override");}public boolean getBlockNetworkLoads(){return block;}
        };
        public ServiceWorkerWebSettings getServiceWorkerWebSettings(){return settings;}
        public void setServiceWorkerClient(ServiceWorkerClient client){if(client!=null)unavailable("service worker request interception");}
    };
    @Override public synchronized WebViewDatabase getWebViewDatabase(Context context){return databases.computeIfAbsent(context.getApplicationContext(),WebKitDatabase::new);}
    @Override public WebIconDatabase getWebIconDatabase(){return icons;}
    private final WebIconDatabase icons=new WebIconDatabase(){
        public void open(String path){}public void close(){}public void removeAllIcons(){}
        public void requestIconForPageUrl(String url,IconListener listener){complete(()->listener.onReceivedIcon(url,null));}
        public void bulkRequestIconForPageUrl(ContentResolver resolver,String where,IconListener listener){unavailable("legacy favicon database");}
        public void retainIconForPageUrl(String url){}public void releaseIconForPageUrl(String url){}
    };
    @Override public Statics getStatics(){return statics;}
    private final Statics statics=new Statics(){
        public String findAddress(String address){return null;}
        public String getDefaultUserAgent(Context context){return userAgent();}
        public void freeMemoryForTests(){}
        public void setWebContentsDebuggingEnabled(boolean enabled){if(enabled)unavailable("remote browser debugging");}
        public void clearClientCertPreferences(Runnable callback){complete(callback);}
        public void enableSlowWholeDocumentDraw(){}
        public Uri[] parseFileChooserResult(int result,Intent data){if(result!=Activity.RESULT_OK||data==null)return null;ClipData clips=data.getClipData();if(clips!=null){Uri[] uris=new Uri[clips.getItemCount()];for(int i=0;i<uris.length;i++)uris[i]=clips.getItemAt(i).getUri();return uris;}return data.getData()==null?null:new Uri[]{data.getData()};}
        public void initSafeBrowsing(Context context,ValueCallback<Boolean> callback){unavailable("Android Safe Browsing service");if(callback!=null)complete(()->callback.onReceiveValue(false));}
        public void setSafeBrowsingWhitelist(List<String> values,ValueCallback<Boolean> callback){if(callback!=null)complete(()->callback.onReceiveValue(false));}
        public Uri getSafeBrowsingPrivacyPolicyUrl(){return null;}
    };
}
