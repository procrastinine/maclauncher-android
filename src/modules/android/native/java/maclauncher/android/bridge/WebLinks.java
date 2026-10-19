package maclauncher.android.bridge;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import java.net.URI;
import java.nio.charset.StandardCharsets;

/** HTTP(S) requests use the existing launcher, which independently validates
 * every request and enforces its captured launch policy outside the sandbox. */
public final class WebLinks {
    private static volatile boolean configured,allowed,closed;
    private static final java.util.regex.Pattern CONTROLS=java.util.regex.Pattern.compile("%(?:0[0-9a-f]|1[0-9a-f]|7f)",java.util.regex.Pattern.CASE_INSENSITIVE);
    private WebLinks(){}
    public static synchronized void hostConfigure(boolean enabled){
        if(configured||closed)throw new IllegalStateException("Web-link policy is already configured");
        boolean usable=configureNative(enabled);allowed=enabled&&usable;configured=true;
    }
    public static synchronized void hostShutdown(){
        closed=true;allowed=false;if(configured)closeNative();
    }
    static String url(Intent intent){
        // Original PackageManager resolution runs before this fallback. Android
        // routing metadata does not make an unresolved HTTP(S) URL unsafe; the
        // host independently validates it and asks for browser approval.
        if(!"android.intent.action.VIEW".equals(intent.getAction()))return null;
        String text=intent.getDataString();if(text==null||text.isEmpty()||text.length()>8192)return null;
        for(int i=0;i<text.length();i++){char c=text.charAt(i);if(c<=32||c==127||c=='\\')return null;}
        if(CONTROLS.matcher(text).find())return null;
        try{
            URI uri=new URI(text);
            if(!"http".equalsIgnoreCase(uri.getScheme())&&!"https".equalsIgnoreCase(uri.getScheme())||uri.getRawUserInfo()!=null||uri.getRawAuthority()==null||uri.getRawAuthority().isEmpty())return null;
            // URI accepts Unicode host names; the parent performs final URL
            // normalization before calling the platform browser service.
            if(uri.getRawAuthority().indexOf('@')>=0)return null;
            // URI treats a Unicode authority as registry-based, so getPort()
            // returns -1 even with a valid explicit port. URL parses that same
            // authority without resolving the host; the parent normalizes it.
            int port=uri.toURL().getPort();
            if(port>65535||uri.getRawAuthority().indexOf(':')>=0&&port<0&&!uri.getRawAuthority().startsWith("["))return null;
            String ascii=uri.toASCIIString();return ascii.getBytes(StandardCharsets.UTF_8).length<=8192?ascii:null;
        }catch(java.net.URISyntaxException|java.net.MalformedURLException error){return null;}
    }
    public static void open(Intent intent){
        java.util.Objects.requireNonNull(intent,"intent");
        String target=url(intent);
        if(target==null)throw new ActivityNotFoundException("Only HTTP and HTTPS web links can open in the Mac browser");
        // The host owns acceptance of a valid browser request. Declining one,
        // blocking network access or retiring its channel does not remove the
        // Android VIEW destination or throw into an application's render loop.
        if(!configured||!allowed||closed)return;
        openNative(target.getBytes(StandardCharsets.UTF_8));
    }
    private static native boolean configureNative(boolean allowed);
    private static native boolean openNative(byte[] url);
    private static native void closeNative();
}
