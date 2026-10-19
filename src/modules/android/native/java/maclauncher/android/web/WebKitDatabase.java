package maclauncher.android.web;

import android.content.*;
import android.webkit.WebViewDatabase;
import org.json.*;

/** Application-private HTTP authentication preferences for the browser provider. */
final class WebKitDatabase extends WebViewDatabase {
    private final SharedPreferences auth;
    WebKitDatabase(Context context){auth=context.getSharedPreferences("webkit-http-auth",Context.MODE_PRIVATE);}
    private static String key(String host,String realm){return new JSONArray().put(host).put(realm).toString();}
    @Override public boolean hasUsernamePassword(){return false;}
    @Override public void clearUsernamePassword(){}
    @Override public boolean hasHttpAuthUsernamePassword(){return !auth.getAll().isEmpty();}
    @Override public void clearHttpAuthUsernamePassword(){auth.edit().clear().apply();}
    @Override public void setHttpAuthUsernamePassword(String host,String realm,String username,String password){auth.edit().putString(key(host,realm),new JSONArray().put(username).put(password).toString()).apply();}
    @Override public String[] getHttpAuthUsernamePassword(String host,String realm){String value=auth.getString(key(host,realm),null);if(value==null)return null;try{JSONArray data=new JSONArray(value);return new String[]{data.optString(0,null),data.optString(1,null)};}catch(JSONException e){return null;}}
    @Override public boolean hasFormData(){return false;}
    @Override public void clearFormData(){}
}
