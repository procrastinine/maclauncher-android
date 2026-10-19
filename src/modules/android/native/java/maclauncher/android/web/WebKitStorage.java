package maclauncher.android.web;

import android.os.*;
import android.webkit.*;
import java.util.*;
import java.util.function.Consumer;
import org.json.*;

/** Website data ownership belongs to WebKit's private application store. */
final class WebKitStorage extends WebStorage {
    private static final class StoredOrigin extends Origin {StoredOrigin(String origin,long quota,long usage){super(origin,quota,usage);}}
    private void command(String operation,String origin,Consumer<String> callback){
        JSONObject data=new JSONObject();try{if(origin!=null)data.put("origin",origin);}catch(JSONException e){throw new IllegalArgumentException(e);}
        Looper loop=Looper.myLooper();Handler handler=loop==null?null:new Handler(loop);
        WebKitNative.storageCommand(operation,data.toString(),(json,error)->{if(error!=null)System.err.println("[android-webkit] Website data operation: "+error);if(callback!=null){Runnable done=()->callback.accept(error==null?json:null);if(handler==null)WebKitFactory.complete(done);else handler.post(done);}});
    }
    @Override public void getOrigins(ValueCallback<Map> callback){command("origins",null,json->{Map<String,Origin> values=new LinkedHashMap<>();if(json!=null)try{JSONArray array=new JSONArray(json);for(int i=0;i<array.length();i++){JSONObject item=array.getJSONObject(i);String origin=item.getString("origin");values.put(origin,new StoredOrigin(origin,item.optLong("quota"),item.optLong("usage")));}}catch(JSONException error){System.err.println("[android-webkit] Website data response: "+error.getMessage());}callback.onReceiveValue(values);});}
    private void quantity(String operation,String origin,ValueCallback<Long> callback){command(operation,origin,json->{long value=0;if(json!=null&&!json.equals("null"))try{value=Long.parseLong(json);}catch(NumberFormatException ignored){}callback.onReceiveValue(value);});}
    @Override public void getUsageForOrigin(String origin,ValueCallback<Long> callback){quantity("usage",origin,callback);}
    @Override public void getQuotaForOrigin(String origin,ValueCallback<Long> callback){quantity("quota",origin,callback);}
    @Override public void setQuotaForOrigin(String origin,long quota){WebKitFactory.unavailable("website storage quota override");}
    @Override public void deleteOrigin(String origin){command("deleteOrigin",origin,null);}
    @Override public void deleteAllData(){command("deleteAll",null,null);}
}
