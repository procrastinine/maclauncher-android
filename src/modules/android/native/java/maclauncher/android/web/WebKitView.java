package maclauncher.android.web;

import android.graphics.*;
import android.os.*;
import android.view.*;
import android.view.inputmethod.*;
import android.text.InputType;
import android.webkit.*;
import java.lang.reflect.*;
import java.nio.ByteBuffer;
import java.util.*;
import maclauncher.android.framework.HostLooper;
import org.json.*;

/** A native browser engine behind Android's original WebView and provider interfaces. */
public final class WebKitView implements InvocationHandler,AutoCloseable {
    private final WebView view;
    private final WebView.PrivateAccess access;
    private final WebKitSettings settings;
    private final Handler main;
    private final WebViewProvider provider;
    private final WebViewProvider.ViewDelegate views;
    private final WebViewProvider.ScrollDelegate scroll;
    private final Map<Long,ValueCallback<String>> scripts=new HashMap<>();
    private final Map<String,Object> interfaces=new HashMap<>();
    private final Map<Long,Port> ports=new HashMap<>();
    private final ArrayList<WebView.VisualStateCallback> visual=new ArrayList<>();
    private final ArrayList<Long> visualIds=new ArrayList<>();
    private final Runnable retirement=this::close;
    private long handle,request;
    private boolean closed,attached,dirty,pending,scheduled,paused;
    private Bitmap bitmap;
    private WebViewClient client=new WebViewClient();
    private WebChromeClient chrome;
    private DownloadListener download;
    private String url,originalUrl,title;
    private int progress,historyIndex=-1,contentWidth,contentHeight;
    private boolean horizontalOverlay,verticalOverlay;
    private float scale=1;
    private int rendererPriority=WebView.RENDERER_PRIORITY_IMPORTANT;
    private boolean priorityWaived;
    private WebViewRenderProcessClient rendererClient;
    private WebView.FindListener findListener;
    private int matches,activeMatch,historyFloor;
    private String findText="";
    private String editorText="",editorIdentity="";
    private int selectionStart,selectionEnd,composingStart=-1,composingEnd=-1;
    private long editorRevision;
    private WebKitEditor inputConnection;
    private boolean editable,password;
    private final Set<String> diagnostics=new HashSet<>();
    private String[] historyUrls=new String[0],historyTitles=new String[0];
    private final Paint paint=new Paint(Paint.FILTER_BITMAP_FLAG);
    private WebKitView(WebView view,WebView.PrivateAccess access){
        this.view=Objects.requireNonNull(view);this.access=Objects.requireNonNull(access);main=new Handler(view.getContext().getMainLooper());
        if(Looper.myLooper()!=main.getLooper())throw new IllegalStateException("WebView requires its Android window thread");
        provider=proxy(WebViewProvider.class);views=proxy(WebViewProvider.ViewDelegate.class);scroll=proxy(WebViewProvider.ScrollDelegate.class);
        handle=WebKitNative.create(this);settings=new WebKitSettings(this::setting);HostLooper.addShutdownListener(main.getLooper(),retirement);
    }
    public static WebViewProvider create(WebView view,WebView.PrivateAccess access){return new WebKitView(view,access).provider;}
    @SuppressWarnings("unchecked") private <T>T proxy(Class<T> type){return(T)Proxy.newProxyInstance(type.getClassLoader(),new Class<?>[]{type},this);}
    private void setting(String name,Object value){if(handle!=0){String encoded=new JSONArray().put(value==null?JSONObject.NULL:value).toString();WebKitNative.setting(handle,name,encoded.substring(1,encoded.length()-1));}}
    private void visibility(){WebKitNative.visibility(handle,attached&&view.isShown()&&!paused);WebKitNative.focus(handle,attached&&view.isShown()&&!paused&&view.hasFocus());}
    private float density(){return view.getResources().getDisplayMetrics().density;}
    private void size(int width,int height){WebKitNative.size(handle,Math.max(1,width),Math.max(1,height),density());}
    private void dirty(){if(closed)return;dirty=true;if(!scheduled&&attached&&!paused){scheduled=true;Choreographer.getInstance().postFrameCallback(time->{scheduled=false;if(closed||!attached||paused||!dirty||pending)return;dirty=false;pending=true;WebKitNative.snapshot(handle);});}}
    private void diagnostic(String operation){if(diagnostics.add(operation))System.err.println("[android-webkit] Platform operation is unavailable: "+operation);}
    private void command(String name,Map<String,?> values,ValueCallback<String> callback){long id=callback==null?0:++request;if(callback!=null)scripts.put(id,callback);WebKitNative.command(handle,name,new JSONObject(values).toString(),id);}
    private void load(String next,Map<String,String> headers,byte[] body){Objects.requireNonNull(next);if(next.regionMatches(true,0,"javascript:",0,11)){WebKitNative.script(handle,next.substring(11),0);return;}originalUrl=next;Map<String,String> supplied=headers==null?Map.of():headers;String[] keys=supplied.keySet().toArray(String[]::new),values=Arrays.stream(keys).map(supplied::get).toArray(String[]::new);WebKitNative.load(handle,next,keys,values,body);}
    @Override public Object invoke(Object ignored,Method method,Object[] supplied)throws Throwable{
        Object[] a=supplied==null?new Object[0]:supplied;String name=method.getName();
        if(method.getDeclaringClass()==Object.class)return switch(name){case "toString"->"WebKit Android browser provider";case "hashCode"->System.identityHashCode(this);case "equals"->ignored==a[0];default->throw new AssertionError(name);};
        if(name.equals("destroy")){close();return null;}if(closed){if(name.equals("getViewDelegate"))return views;if(name.equals("getScrollDelegate"))return scroll;if(name.equals("getSettings"))return settings;if(name.equals("getHandler")||name.equals("findFocus"))return a[0];if(name.equals("onDetachedFromWindow")){attached=false;return null;}Class<?> result=method.getReturnType();if(result==void.class)return null;if(result==boolean.class)return false;if(result==int.class)return 0;if(result==float.class)return 0f;if(result==long.class)return 0L;if(result==double.class)return 0d;return null;}
        switch(name){
            case "getViewDelegate":return views;case "getScrollDelegate":return scroll;case "getSettings":return settings;
            case "init":view.setWillNotDraw(false);if(a[0] instanceof Map<?,?> initial){for(var entry:initial.entrySet())if(entry.getKey() instanceof String key&&entry.getValue()!=null)interfaces.put(key,entry.getValue());publishInterfaces();}return null;
            case "setHorizontalScrollbarOverlay":horizontalOverlay=(Boolean)a[0];return null;case "setVerticalScrollbarOverlay":verticalOverlay=(Boolean)a[0];return null;case "overlayHorizontalScrollbar":return horizontalOverlay;case "overlayVerticalScrollbar":return verticalOverlay;case "getVisibleTitleHeight":return 0;
            case "loadUrl":load((String)a[0],a.length>1?(Map<String,String>)a[1]:Map.of(),null);return null;
            case "postUrl":load((String)a[0],Map.of("Content-Type","application/x-www-form-urlencoded"),(byte[])a[1]);return null;
            case "loadData":{String data=(String)a[0];if("base64".equals(a[2]))data=new String(Base64.getDecoder().decode(data),java.nio.charset.StandardCharsets.UTF_8);WebKitNative.html(handle,data,"about:blank");return null;}
            case "loadDataWithBaseURL":originalUrl=(String)a[0];WebKitNative.html(handle,(String)a[1],originalUrl);return null;
            case "evaluateJavaScript":{long id=++request;if(a[1]!=null)scripts.put(id,(ValueCallback<String>)a[1]);WebKitNative.script(handle,(String)a[0],id);return null;}
            case "reload":WebKitNative.reload(handle);return null;case "stopLoading":WebKitNative.stop(handle);return null;
            case "goBack":if(historyIndex>0)WebKitNative.navigate(handle,-1);return null;case "goForward":if(historyIndex+1<historyUrls.length)WebKitNative.navigate(handle,1);return null;case "goBackOrForward":{int delta=(Integer)a[0],next=historyIndex+delta;if(next>=0&&next<historyUrls.length)WebKitNative.navigate(handle,delta);return null;}
            case "canGoBack":return historyIndex>0;case "canGoForward":return historyIndex+1<historyUrls.length;case "canGoBackOrForward":{int next=historyIndex+(Integer)a[0];return next>=0&&next<historyUrls.length;}
            case "getUrl":return url;case "getOriginalUrl":return originalUrl;case "getTitle":return title;case "getProgress":return progress;
            case "getFavicon":case "getTouchIconUrl":case "getCertificate":return null;
            case "setWebViewClient":client=a[0]==null?new WebViewClient():(WebViewClient)a[0];return null;case "getWebViewClient":return client;
            case "setWebChromeClient":chrome=(WebChromeClient)a[0];return null;case "getWebChromeClient":return chrome;
            case "setDownloadListener":download=(DownloadListener)a[0];return null;
            case "addJavascriptInterface":if(a[0]!=null){interfaces.put((String)a[1],a[0]);publishInterfaces();}return null;
            case "removeJavascriptInterface":interfaces.remove((String)a[0]);publishInterfaces();return null;
            case "copyBackForwardList":return new History(historyUrls,historyTitles,historyIndex);
            case "saveState":{byte[] state=WebKitNative.state(handle);if(state==null)return null;((Bundle)a[0]).putByteArray("maclauncher.webkit.state",state);return new History(historyUrls,historyTitles,historyIndex);}
            case "restoreState":{byte[] state=((Bundle)a[0]).getByteArray("maclauncher.webkit.state");return state!=null&&WebKitNative.restore(handle,state)?new History(historyUrls,historyTitles,historyIndex):null;}
            case "clearHistory":historyFloor+=Math.max(0,historyIndex);historyUrls=url==null?new String[0]:new String[]{url};historyTitles=title==null?new String[]{""}:new String[]{title};historyIndex=url==null?-1:0;return null;
            case "clearCache":command("clearCache",Map.of("disk",a[0]),null);return null;
            case "clearFormData":WebKitNative.script(handle,"for(const form of document.forms)form.reset()",0);return null;
            case "clearSslPreferences":command("clearCredentials",Map.of(),null);return null;
            case "pauseTimers":command("pause",Map.of("paused",true),null);diagnostic("JavaScript timer suspension; media and animations are paused");return null;
            case "resumeTimers":command("pause",Map.of("paused",false),null);return null;
            case "setFindListener":findListener=(WebView.FindListener)a[0];return null;
            case "findAll":case "findAllAsync":{findText=a[0]==null?"":(String)a[0];String quoted=JSONObject.quote(findText);long id=++request;scripts.put(id,result->{try{matches=Integer.parseInt(result);}catch(Exception ignoredResult){matches=0;}activeMatch=0;if(findListener!=null)findListener.onFindResultReceived(activeMatch,matches,true);});WebKitNative.script(handle,"(()=>{const s=document.body?.innerText||'',q="+quoted+";return q?s.toLowerCase().split(q.toLowerCase()).length-1:0})()",id);command("find",Map.of("text",findText,"backwards",false),null);return name.equals("findAll")?matches:null;}
            case "findNext":command("find",Map.of("text",findText,"backwards",!(Boolean)a[0]),null);if(matches>0)activeMatch=Math.floorMod(activeMatch+((Boolean)a[0]?1:-1),matches);if(findListener!=null)findListener.onFindResultReceived(activeMatch,matches,true);return null;
            case "clearMatches":findText="";matches=0;command("find",Map.of("text","","backwards",false),null);return null;
            case "saveWebArchive":{String filename=(String)a[0];ValueCallback<String> callback=a.length>2?(ValueCallback<String>)a[2]:null;command("archive",Map.of("path",filename),callback);return null;}
            case "getHitTestResult":return new WebView.HitTestResult();
            case "requestFocusNodeHref":case "requestImageRef":{Message message=(Message)a[0];long id=++request;scripts.put(id,result->{try{JSONObject info=new JSONObject(result);Bundle values=message.getData();values.putString("url",info.optString("url",null));values.putString("title",info.optString("title",null));values.putString("src",info.optString("src",null));}catch(JSONException ignoredResult){}message.sendToTarget();});WebKitNative.script(handle,"(()=>{const e=document.activeElement;return {url:e?.href||null,title:e?.textContent||null,src:e?.src||null}})()",id);return null;}
            case "savePicture":case "restorePicture":return false;
            case "showFindDialog":case "invokeZoomPicker":case "getZoomControls":diagnostic(name);return method.getReturnType()==boolean.class?false:null;
            case "notifyFindDialogDismissed":case "setMapTrackballToArrowKeys":case "setPictureListener":return null;
            case "getScale":return scale*density();case "setInitialScale":if((Integer)a[0]>0){scale=(Integer)a[0]/100f;setting("PageZoom",scale);}return null;
            case "canZoomIn":case "canZoomOut":return settings.supportZoom();
            case "zoomBy":case "zoomIn":case "zoomOut":if(!settings.supportZoom())return false;scale*=name.equals("zoomBy")?(Float)a[0]:name.equals("zoomIn")?1.25f:.8f;setting("PageZoom",scale);return true;
            case "getContentWidth":return contentWidth;case "getContentHeight":return contentHeight;
            case "capturePicture":{Picture picture=new Picture();if(bitmap!=null){Canvas canvas=picture.beginRecording(bitmap.getWidth(),bitmap.getHeight());canvas.drawBitmap(bitmap,0,0,null);picture.endRecording();}return picture;}
            case "pageUp":case "pageDown":WebKitNative.script(handle,"window.scrollBy(0,"+(name.equals("pageUp")?-1:1)+"*"+((Boolean)a[0]?"document.documentElement.scrollHeight":"innerHeight")+")",0);return bitmap!=null;
            case "flingScroll":WebKitNative.script(handle,"window.scrollBy({left:"+a[0]+"/3,top:"+a[1]+"/3,behavior:'smooth'})",0);return null;
            case "setNetworkAvailable":WebKitNative.script(handle,"dispatchEvent(new Event('"+((Boolean)a[0]?"online":"offline")+"'))",0);return null;
            case "setRendererPriorityPolicy":rendererPriority=(Integer)a[0];priorityWaived=(Boolean)a[1];return null;case "getRendererRequestedPriority":return rendererPriority;case "getRendererPriorityWaivedWhenNotVisible":return priorityWaived;
            case "setWebViewRenderProcessClient":rendererClient=(WebViewRenderProcessClient)a[1];return null;case "getWebViewRenderProcessClient":return rendererClient;case "getWebViewRenderProcess":return null;
            case "createWebMessageChannel":{Port first=new Port(++request),second=new Port(++request);ports.put(first.id,first);ports.put(second.id,second);command("portCreate",Map.of("first",first.id,"second",second.id),null);return new WebMessagePort[]{first,second};}
            case "postMessageToMainFrame":{WebMessage message=(WebMessage)a[0];command("portTransfer",Map.of("data",message.getData(),"origin",String.valueOf(a[1]),"ports",transferred(message)),null);return null;}
            case "documentHasImages":{Message message=(Message)a[0];long id=++request;scripts.put(id,result->{message.arg1="true".equals(result)?1:0;message.sendToTarget();});WebKitNative.script(handle,"document.images.length>0",id);return null;}
            case "freeMemory":if(!attached&&bitmap!=null){bitmap.recycle();bitmap=null;}return null;
            case "onAttachedToWindow":attached=true;visibility();size(view.getWidth(),view.getHeight());dirty();return null;
            case "onDetachedFromWindow":attached=false;visibility();return null;
            case "onSizeChanged":size((Integer)a[0],(Integer)a[1]);dirty();return null;
            case "onConfigurationChanged":size(view.getWidth(),view.getHeight());dirty();return null;
            case "onFocusChanged":WebKitNative.focus(handle,(Boolean)a[0]);dirty();return null;
            case "onDraw":if(bitmap!=null)((Canvas)a[0]).drawBitmap(bitmap,null,new Rect(0,0,view.getWidth(),view.getHeight()),paint);return null;
            case "onMeasure":access.setMeasuredDimension(View.getDefaultSize(view.getMinimumWidth(),(Integer)a[0]),View.getDefaultSize(view.getMinimumHeight(),(Integer)a[1]));return null;
            case "setFrame":return access.super_setFrame((Integer)a[0],(Integer)a[1],(Integer)a[2],(Integer)a[3]);
            case "setLayoutParams":access.super_setLayoutParams((ViewGroup.LayoutParams)a[0]);return null;
            case "onTouchEvent":{MotionEvent event=(MotionEvent)a[0];if(event.getActionMasked()==MotionEvent.ACTION_DOWN)view.requestFocus();WebKitNative.touch(handle,event.getActionMasked(),event.getX(),event.getY());dirty();return true;}
            case "dispatchKeyEvent":{KeyEvent event=(KeyEvent)a[0];if(event.getKeyCode()==KeyEvent.KEYCODE_BACK)return false;String chars=event.getCharacters();int unicode=event.getUnicodeChar();if(chars==null&&unicode>0)chars=new String(Character.toChars(unicode));WebKitNative.key(handle,event.getAction(),event.getKeyCode(),chars,event.getMetaState());dirty();return true;}
            case "onKeyDown":case "onKeyUp":case "onKeyMultiple":return false;
            case "requestFocus":return access.super_requestFocus((Integer)a[0],(Rect)a[1]);
            case "performAccessibilityAction":return access.super_performAccessibilityAction((Integer)a[0],(Bundle)a[1]);
            case "performLongClick":return access.super_performLongClick();
            case "onHoverEvent":return access.super_onHoverEvent((MotionEvent)a[0]);
            case "onGenericMotionEvent":return access.super_onGenericMotionEvent((MotionEvent)a[0]);
            case "setBackgroundColor":setting("BackgroundColor",a[0]);return null;
            case "onDrawVerticalScrollBar":access.super_onDrawVerticalScrollBar((Canvas)a[0],(android.graphics.drawable.Drawable)a[1],(Integer)a[2],(Integer)a[3],(Integer)a[4],(Integer)a[5]);return null;
            case "onOverScrolled":access.super_scrollTo((Integer)a[0],(Integer)a[1]);return null;
            case "onApplyWindowInsets":return access.super_onApplyWindowInsets((WindowInsets)a[0]);
            case "onTrackballEvent":case "onDragEvent":case "requestChildRectangleOnScreen":return false;
            case "onCheckIsTextEditor":return true;
            case "onCreateInputConnection":{EditorInfo info=(EditorInfo)a[0];info.inputType=editable?InputType.TYPE_CLASS_TEXT|(password?InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD:InputType.TYPE_TEXT_VARIATION_WEB_EDIT_TEXT):InputType.TYPE_NULL;info.initialSelStart=selectionStart;info.initialSelEnd=selectionEnd;info.imeOptions=EditorInfo.IME_FLAG_NO_EXTRACT_UI;return inputConnection();}
            case "getHandler":return a[0];case "findFocus":return a[0];
            case "onPause":paused=true;visibility();return null;case "onResume":paused=false;visibility();dirty();return null;case "isPaused":return paused;
            case "onWindowVisibilityChanged":case "onVisibilityChanged":visibility();dirty();return null;
            case "insertVisualStateCallback":visualIds.add((Long)a[0]);visual.add((WebView.VisualStateCallback)a[1]);dirty();return null;
            case "clearView":if(bitmap!=null){bitmap.recycle();bitmap=null;}view.invalidate();return null;
            case "computeHorizontalScrollOffset":return view.getScrollX();case "computeVerticalScrollOffset":return view.getScrollY();
            case "computeHorizontalScrollRange":return Math.max(view.getWidth(),(int)Math.ceil(contentWidth*scale*density()));
            case "computeVerticalScrollRange":return Math.max(view.getHeight(),(int)Math.ceil(contentHeight*scale*density()));
            case "computeVerticalScrollExtent":return view.getHeight();
            case "shouldDelayChildPressedState":return false;case "isPrivateBrowsingEnabled":return true;
            case "getAccessibilityNodeProvider":return null;
            case "setOverScrollMode":case "setScrollBarStyle":case "setLayerType":case "onInitializeAccessibilityNodeInfo":case "onInitializeAccessibilityEvent":case "onProvideVirtualStructure":case "onActivityResult":case "onWindowFocusChanged":case "preDispatchDraw":case "onScrollChanged":case "onStartTemporaryDetach":case "onFinishTemporaryDetach":case "computeScroll":return null;
        }
        if(method.isDefault())return java.lang.invoke.MethodHandles.privateLookupIn(method.getDeclaringClass(),java.lang.invoke.MethodHandles.lookup())
            .unreflectSpecial(method,method.getDeclaringClass()).bindTo(ignored).invokeWithArguments(a==null?new Object[0]:a);
        diagnostic(method.toGenericString());Class<?> result=method.getReturnType();if(result==void.class)return null;if(result==boolean.class)return false;if(result==int.class)return 0;if(result==float.class)return 0f;if(result==long.class)return 0L;if(result==double.class)return 0d;return null;
    }
    /** WebKit publishes document state; original Android code edits its Editable. */
    public void editorState(String json){
        if(closed)return;try{JSONObject state=new JSONObject(json);
            String identity=state.optString("identity")+":"+state.optLong("element");
            boolean changed=!identity.equals(editorIdentity),wasEditable=editable,wasPassword=password;
            if(changed){if(inputConnection!=null)inputConnection.retire();inputConnection=null;editorRevision=0;editorIdentity=identity;}
            long revision=state.optLong("revision");if(revision<editorRevision)return;
            editable=state.optBoolean("editable");password=state.optBoolean("password");editorText=state.optString("text");selectionStart=state.optInt("start");selectionEnd=state.optInt("end");
            composingStart=state.optInt("composingStart",-1);composingEnd=state.optInt("composingEnd",-1);editorRevision=revision;
            if(inputConnection!=null)inputConnection.synchronize(editorSnapshot(),revision);
            if(view.hasFocus()&&(changed||wasEditable!=editable||wasPassword!=password))view.getContext().getSystemService(InputMethodManager.class).restartInput(view);
        }catch(JSONException failure){throw new IllegalStateException("WebKit editor state is invalid",failure);}
    }
    private WebKitEditor.State editorSnapshot(){return new WebKitEditor.State(editorText,selectionStart,selectionEnd,composingStart,composingEnd);}
    private WebKitEditor inputConnection(){
        if(inputConnection!=null)inputConnection.retire();
        return inputConnection=new WebKitEditor(view,main,new WebKitEditor.Target(){
            public void update(WebKitEditor.State before,WebKitEditor.State after,long revision){
                if(closed)return;editorText=after.text();selectionStart=after.start();selectionEnd=after.end();composingStart=after.composingStart();composingEnd=after.composingEnd();editorRevision=revision;
                String metadata=new JSONObject(Map.of("beforeStart",before.start(),"beforeEnd",before.end(),"beforeComposingStart",before.composingStart(),"beforeComposingEnd",before.composingEnd(),"start",after.start(),"end",after.end(),"composingStart",after.composingStart(),"composingEnd",after.composingEnd(),"revision",revision,"identity",editorIdentity)).toString();
                WebKitNative.editor(handle,before.text(),after.text(),metadata);dirty();
            }
            public boolean action(WebKitEditor editor,int id){
                if(closed)return false;
                if(id==android.R.id.selectAll)return editor.setSelection(0,editor.getEditable().length());
                var clipboard=view.getContext().getSystemService(android.content.ClipboardManager.class);
                if(id==android.R.id.copy||id==android.R.id.cut){CharSequence selected=editor.getSelectedText(0);if(password||selected==null||selected.length()==0)return false;clipboard.setPrimaryClip(android.content.ClipData.newPlainText("",selected));if(id==android.R.id.cut){var text=editor.getEditable();int start=android.text.Selection.getSelectionStart(text),end=android.text.Selection.getSelectionEnd(text);text.delete(Math.min(start,end),Math.max(start,end));}return true;}
                if(id==android.R.id.paste){var clip=clipboard.getPrimaryClip();return clip!=null&&clip.getItemCount()>0&&editor.commitText(clip.getItemAt(0).coerceToText(view.getContext()),1);}
                return false;
            }
            public boolean key(KeyEvent event){if(closed)return false;WebKitNative.key(handle,event.getAction(),event.getKeyCode(),event.getCharacters(),event.getMetaState());return true;}
        },editorSnapshot(),editorRevision);
    }
    /** Called only by the actual WebKit completion on the existing window thread. */
    public void frame(ByteBuffer pixels,int width,int height,String error){pending=false;if(closed)return;if(error!=null){System.err.println("[android-webkit] Snapshot: "+error);return;}if(bitmap==null||bitmap.getWidth()!=width||bitmap.getHeight()!=height){if(bitmap!=null)bitmap.recycle();bitmap=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);}pixels.rewind();bitmap.copyPixelsFromBuffer(pixels);view.invalidate();for(int i=0;i<visual.size();i++)visual.get(i).onComplete(visualIds.get(i));visual.clear();visualIds.clear();if(dirty)dirty();}
    public void changed(){dirty();}
    /** Document extent is in CSS pixels; the captured Bitmap is only the viewport. */
    public void documentDimensions(int width,int height){if(closed)return;contentWidth=Math.max(0,width);contentHeight=Math.max(0,height);}
    public void scriptResult(long id,String value,String error){ValueCallback<String> callback=scripts.remove(id);if(callback!=null)callback.onReceiveValue(error==null?value:"null");if(error!=null)System.err.println("[android-webkit] JavaScript: "+error);dirty();}
    public boolean navigateRequest(String next,boolean mainFrame,boolean gesture,boolean redirect,String method,String encodedHeaders)throws JSONException{
        if(closed)return true;
        Map<String,String> headers=new LinkedHashMap<>();
        if(encodedHeaders!=null){JSONObject values=new JSONObject(encodedHeaders);for(Iterator<String> keys=values.keys();keys.hasNext();){String key=keys.next();headers.put(key,values.getString(key));}}
        WebResourceRequest request=new WebResourceRequest(){
            public android.net.Uri getUrl(){return android.net.Uri.parse(next);}
            public boolean isForMainFrame(){return mainFrame;}
            public boolean isRedirect(){return redirect;}
            public boolean hasGesture(){return gesture;}
            public String getMethod(){return method;}
            public Map<String,String> getRequestHeaders(){return headers;}
        };
        // The original request overload also dispatches to clients overriding
        // Android's older String overload. Subframes can carry application
        // messages and retain the same original callback contract.
        return client.shouldOverrideUrlLoading(view,request);
    }
    public void navigation(String state,String next,String text,int value){if(closed)return;url=next;title=text;progress=value;switch(state){case "started":client.onPageStarted(view,next,null);break;case "finished":client.onPageFinished(view,next);break;case "history":client.doUpdateVisitedHistory(view,next,false);break;case "error":client.onReceivedError(view,WebViewClient.ERROR_UNKNOWN,text,next);break;}if(chrome!=null){chrome.onProgressChanged(view,value);if(text!=null)chrome.onReceivedTitle(view,text);}dirty();}
    public void history(String[] urls,String[] titles,int current){historyFloor=Math.min(historyFloor,Math.max(0,current));historyUrls=Arrays.copyOfRange(urls,historyFloor,urls.length);historyTitles=Arrays.copyOfRange(titles,historyFloor,titles.length);historyIndex=current-historyFloor;}
    private List<Long> transferred(WebMessage message){List<Long> result=new ArrayList<>();if(message.getPorts()!=null)for(WebMessagePort value:message.getPorts()){if(!(value instanceof Port port)||port.closed||port.transferred)throw new IllegalStateException("Web message port is unavailable");port.transferred=true;result.add(port.id);}return result;}
    public void portMessage(long id,String message){Port port=ports.get(id);if(port!=null&&!port.closed&&port.callback!=null)port.handler.post(()->{if(!port.closed)port.callback.onMessage(port,new WebMessage(message));});}
    private final class Port extends WebMessagePort {
        final long id;boolean closed,transferred;WebMessageCallback callback;Handler handler=main;Port(long id){this.id=id;}
        public void postMessage(WebMessage message){if(closed||transferred)throw new IllegalStateException("Web message port is closed or transferred");List<Long> transfer=transferred(message);main.post(()->{if(!WebKitView.this.closed)command("portPost",Map.of("id",id,"data",message.getData(),"ports",transfer),null);});}
        public void close(){if(closed)return;closed=true;ports.remove(id);main.post(()->{if(!WebKitView.this.closed)command("portClose",Map.of("id",id),null);});}
        public void setWebMessageCallback(WebMessageCallback value){setWebMessageCallback(value,main);}public void setWebMessageCallback(WebMessageCallback value,Handler target){if(closed||transferred)throw new IllegalStateException("Web message port is closed or transferred");callback=value;handler=target==null?main:target;}
    }
    public byte[] resource(String address)throws java.io.IOException {
        android.net.Uri uri=android.net.Uri.parse(address);String path=uri.getPath();if(path==null)throw new java.io.FileNotFoundException(address);while(path.startsWith("/"))path=path.substring(1);
        if(Arrays.asList(path.split("/")).contains(".."))throw new java.io.FileNotFoundException(address);
        try(java.io.InputStream input=uri.getScheme().equals("android-asset")?view.getContext().getAssets().open(path):view.getContext().getResources().openRawResource(view.getContext().getResources().getIdentifier(path.replaceFirst("\\.[^.]+$",""),null,view.getContext().getPackageName()))){return input.readAllBytes();}
    }
    private void publishInterfaces(){Map<String,JSONArray> names=new LinkedHashMap<>();for(var entry:interfaces.entrySet()){JSONArray methods=new JSONArray();for(Method method:entry.getValue().getClass().getMethods())if(method.isAnnotationPresent(JavascriptInterface.class)&&!Modifier.isStatic(method.getModifiers()))methods.put(method.getName());names.put(entry.getKey(),methods);}WebKitNative.interfaces(handle,new JSONObject(names).toString());}
    public String invokeInterface(String name,String methodName,String encoded){
        try{Object object=interfaces.get(name);if(object==null)throw new IllegalArgumentException("JavaScript interface is absent");JSONArray args=new JSONArray(encoded);for(Method method:object.getClass().getMethods())if(method.getName().equals(methodName)&&method.isAnnotationPresent(JavascriptInterface.class)&&!Modifier.isStatic(method.getModifiers())&&method.getParameterCount()==args.length()){
            Class<?>[] types=method.getParameterTypes();Object[] values=new Object[types.length];for(int i=0;i<types.length;i++)values[i]=argument(types[i],args.opt(i));
            method.setAccessible(true);Object result=method.invoke(object,values);return new JSONObject(Map.of("ok",true,"value",result==null?JSONObject.NULL:result)).toString();
        }throw new NoSuchMethodException(name+"."+methodName);}catch(Throwable failure){Throwable cause=failure instanceof InvocationTargetException?failure.getCause():failure;return new JSONObject(Map.of("ok",false,"error",cause.toString())).toString();}
    }
    private static Object argument(Class<?> type,Object value){if(value==JSONObject.NULL)value=null;if(type==String.class)return value==null?null:String.valueOf(value);if(type==boolean.class||type==Boolean.class)return value instanceof Boolean?value:false;if(type==char.class||type==Character.class)return value instanceof String text&&!text.isEmpty()?text.charAt(0):'\0';if(type.isArray()){if(!(value instanceof JSONArray array))return null;Object result=Array.newInstance(type.getComponentType(),array.length());for(int i=0;i<array.length();i++)Array.set(result,i,argument(type.getComponentType(),array.opt(i)));return result;}Number number=value instanceof Number?(Number)value:0;if(type==int.class||type==Integer.class)return number.intValue();if(type==long.class||type==Long.class)return number.longValue();if(type==float.class||type==Float.class)return number.floatValue();if(type==double.class||type==Double.class)return number.doubleValue();if(type==short.class||type==Short.class)return number.shortValue();if(type==byte.class||type==Byte.class)return number.byteValue();if(value==null)return null;throw new IllegalArgumentException("Unsupported JavaScript argument type: "+type.getName());}
    public void dialog(long id,String kind,String origin,String message,String value){
        JsResult.ResultReceiver receiver=result->{if(!closed)WebKitNative.dialogResult(handle,id,result.getResult(),result instanceof JsPromptResult?((JsPromptResult)result).getStringResult():null);};
        JsResult result=kind.equals("prompt")?new JsPromptResult(receiver):new JsResult(receiver);boolean handled=chrome!=null&&switch(kind){case "alert"->chrome.onJsAlert(view,origin,message,result);case "confirm"->chrome.onJsConfirm(view,origin,message,result);default->chrome.onJsPrompt(view,origin,message,value,(JsPromptResult)result);};
        if(!handled){android.app.AlertDialog.Builder builder=new android.app.AlertDialog.Builder(view.getContext()).setMessage(message).setOnCancelListener(dialog->result.cancel());if(result instanceof JsPromptResult prompt){android.widget.EditText input=new android.widget.EditText(view.getContext());input.setText(value);builder.setView(input).setPositiveButton(android.R.string.ok,(d,w)->prompt.confirm(input.getText().toString()));}else builder.setPositiveButton(android.R.string.ok,(d,w)->result.confirm());if(!kind.equals("alert"))builder.setNegativeButton(android.R.string.cancel,(d,w)->result.cancel());builder.show();}
    }
    private static final class History extends WebBackForwardList {
        final String[] urls,titles;final int current;History(String[] urls,String[] titles,int current){this.urls=urls.clone();this.titles=titles.clone();this.current=current;}
        public WebHistoryItem getCurrentItem(){return getItemAtIndex(current);}public int getCurrentIndex(){return current;}public int getSize(){return urls.length;}protected WebBackForwardList clone(){return new History(urls,titles,current);}
        public WebHistoryItem getItemAtIndex(int i){if(i<0||i>=urls.length)return null;return new WebHistoryItem(){public int getId(){return i;}public String getUrl(){return urls[i];}public String getOriginalUrl(){return urls[i];}public String getTitle(){return titles[i];}public Bitmap getFavicon(){return null;}protected WebHistoryItem clone(){return getItemAtIndex(i);}};}
    }
    @Override public void close(){if(closed)return;closed=true;if(inputConnection!=null)inputConnection.retire();HostLooper.removeShutdownListener(main.getLooper(),retirement);if(handle!=0){long old=handle;handle=0;WebKitNative.destroy(old);}if(bitmap!=null){bitmap.recycle();bitmap=null;}for(var callback:scripts.values())callback.onReceiveValue("null");scripts.clear();interfaces.clear();for(Port port:ports.values())port.closed=true;ports.clear();visual.clear();visualIds.clear();}
}
