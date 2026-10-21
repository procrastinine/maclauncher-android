import android.webkit.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;
import maclauncher.android.web.WebKitView;
import org.json.*;

public final class WebKitErrorProbe {
    static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    static Field field(String name)throws Exception{Field field=WebKitView.class.getDeclaredField(name);field.setAccessible(true);return field;}
    public static void main(String[] args)throws Exception{
        // Exercise the complete shared callback adapter without creating a window.
        Class<?> unsafe=Class.forName("sun.misc.Unsafe");Field singleton=unsafe.getDeclaredField("theUnsafe");singleton.setAccessible(true);
        WebKitView adapter=(WebKitView)unsafe.getMethod("allocateInstance",Class.class).invoke(singleton.get(null),WebKitView.class);
        String committed="https://committed.example/page",original="https://pending.example/application-load";
        field("url").set(adapter,committed);field("title").set(adapter,"Committed page");field("originalUrl").set(adapter,original);
        List<String> failures=new ArrayList<>(),titles=new ArrayList<>();List<Integer> progress=new ArrayList<>();
        field("client").set(adapter,new WebViewClient(){
            @Override public void onReceivedError(WebView view,int code,String description,String failingUrl){
                // Kotlin's non-null parameter check has this same contract.
                failures.add(Objects.requireNonNull(failingUrl,"failingUrl"));
                Objects.requireNonNull(description,"description");check(code==ERROR_UNKNOWN,"original error code retained");
            }
        });
        field("chrome").set(adapter,new WebChromeClient(){
            @Override public void onReceivedTitle(WebView view,String title){titles.add(title);}
            @Override public void onProgressChanged(WebView view,int value){progress.add(value);}
        });
        JSONArray records=new JSONArray(Files.readString(Path.of(args[0])));
        for(int i=0;i<records.length();i++){
            JSONObject record=records.getJSONObject(i);String address=record.getString("url");
            adapter.navigation("error",address,record.getString("description"),25);
            check(failures.get(i).equals(record.getString("expected")),"the actual NSError/request origin reaches the Android client");
            check(field("url").get(adapter).equals(address),"the reported failed request retains the existing WebView URL update");
            check(field("title").get(adapter).equals("Committed page"),"an error description cannot become a page title");
        }
        check(titles.isEmpty(),"load errors do not emit page titles");
        adapter.navigation("error",null,"Connection failed",30);
        check(failures.get(failures.size()-1).equals(original),"a missing native URL retains the actual application request");
        adapter.navigation("finished","https://loaded.example/","Loaded page",100);
        check(field("url").get(adapter).equals("https://loaded.example/"),"successful navigation updates the URL");
        check(titles.equals(List.of("Loaded page")),"successful navigation retains its title callback");
        check(progress.get(progress.size()-1)==100,"progress callbacks retained");
        int count=failures.size();field("closed").setBoolean(adapter,true);
        adapter.navigation("error",original,"Connection failed",40);
        check(failures.size()==count,"retired browser callbacks remain ignored");
        System.out.println("PASS actual NSError/request URLs reach original Android error callbacks without nulls or document-title corruption");
    }
}
