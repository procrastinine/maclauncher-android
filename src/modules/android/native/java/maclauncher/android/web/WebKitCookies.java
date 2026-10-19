package maclauncher.android.web;

import android.os.*;
import android.webkit.*;
import org.json.*;

/** CookieManager delegates storage and completion to WKHTTPCookieStore. */
final class WebKitCookies extends CookieManager {
    private volatile boolean accept=true,fileCookies=false;
    private void command(String operation,JSONObject data,ValueCallback<Boolean> callback){
        Looper current=Looper.myLooper();Handler handler=current==null?null:new Handler(current);
        WebKitNative.cookieCommand(operation,data.toString(),(json,error)->{
            if(error!=null)System.err.println("[android-webkit] Cookie operation: "+error);
            if(callback!=null){Runnable done=()->callback.onReceiveValue(error==null&&Boolean.parseBoolean(json));if(handler==null)WebKitFactory.complete(done);else handler.post(done);}
        });
    }
    private static JSONObject object(String key,Object value){try{return new JSONObject().put(key,value);}catch(JSONException e){throw new IllegalArgumentException(e);}}
    @Override public void setAcceptCookie(boolean allowed){accept=allowed;command("accept",object("allowed",allowed),null);}
    @Override public boolean acceptCookie(){return accept;}
    @Override public void setAcceptThirdPartyCookies(WebView view,boolean allowed){view.getSettings().setAcceptThirdPartyCookies(allowed);}
    @Override public boolean acceptThirdPartyCookies(WebView view){return view.getSettings().getAcceptThirdPartyCookies();}
    @Override public void setCookie(String url,String value){setCookie(url,value,null);}
    @Override public void setCookie(String url,String value,ValueCallback<Boolean> callback){try{command("set",object("url",url).put("value",value),callback);}catch(JSONException e){throw new IllegalArgumentException(e);}}
    @Override public String getCookie(String url){return WebKitNative.cookies(url);}
    @Override public String getCookie(String url,boolean privateBrowsing){return getCookie(url);}
    @Override public void removeSessionCookie(){removeSessionCookies(null);}
    @Override public void removeSessionCookies(ValueCallback<Boolean> callback){command("removeSession",new JSONObject(),callback);}
    @Override public void removeAllCookie(){removeAllCookies(null);}
    @Override public void removeAllCookies(ValueCallback<Boolean> callback){command("removeAll",new JSONObject(),callback);}
    @Override public boolean hasCookies(){String value=WebKitNative.cookies(null);return value!=null&&!value.isEmpty();}
    @Override public boolean hasCookies(boolean privateBrowsing){return hasCookies();}
    @Override public void removeExpiredCookie(){command("removeExpired",new JSONObject(),null);}
    @Override public void flush(){command("flush",new JSONObject(),null);}
    @Override protected boolean allowFileSchemeCookiesImpl(){return fileCookies;}
    @Override protected void setAcceptFileSchemeCookiesImpl(boolean allowed){fileCookies=allowed;if(allowed)WebKitFactory.unavailable("file-scheme cookies");}
}
