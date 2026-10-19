package maclauncher.android.web;

/** WebKit process, document and image ownership beneath the Android browser provider. */
public final class WebKitNative {
    private WebKitNative(){}
    public static native String initialize(String directory,String appDataRoot,boolean networkAllowed);
    static native long create(WebKitView callbacks);
    static native void destroy(long handle);
    static native void visibility(long handle,boolean visible);
    static native void focus(long handle,boolean focused);
    static native void size(long handle,int width,int height,float density);
    static native void load(long handle,String url,String[] headers,String[] values,byte[] body);
    static native void html(long handle,String html,String baseUrl);
    static native void script(long handle,String script,long request);
    static native void navigate(long handle,int delta);
    static native void reload(long handle);
    static native void stop(long handle);
    static native void snapshot(long handle);
    static native void touch(long handle,int action,float x,float y);
    static native void key(long handle,int action,int key,String text,int modifiers);
    static native void edit(long handle,int operation,String text,int start,int length);
    static native void editor(long handle,String before,String after,String metadata);
    static native void setting(long handle,String name,String json);
    static native void interfaces(long handle,String json);
    static native void dialogResult(long handle,long request,boolean accepted,String value);
    static native byte[] state(long handle);
    static native boolean restore(long handle,byte[] state);
    static native void command(long handle,String operation,String json,long request);
    public static native String cookies(String url);
    public static native void cookieCommand(String operation,String json,WebKitResult completion);
    public static native void storageCommand(String operation,String json,WebKitResult completion);
}
