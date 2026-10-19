package maclauncher.android.web;

import android.webkit.WebSettings;
import java.util.*;
import java.util.function.BiConsumer;

/** Requested browser policy, applied by the WebKit platform engine. */
public final class WebKitSettings extends WebSettings {
  private final Map<String,Object> values=new HashMap<>();
  private final BiConsumer<String,Object> changed;
  public WebKitSettings(BiConsumer<String,Object> changed){this.changed=Objects.requireNonNull(changed);
    values.put("NavDump",false);
    values.put("SupportZoom",true);
    values.put("MediaPlaybackRequiresUserGesture",true);
    values.put("BuiltInZoomControls",false);
    values.put("DisplayZoomControls",true);
    values.put("AllowFileAccess",false);
    values.put("AllowContentAccess",true);
    values.put("LoadWithOverviewMode",false);
    values.put("EnableSmoothTransition",false);
    values.put("UseWebViewBackgroundForOverscrollBackground",false);
    values.put("SaveFormData",true);
    values.put("SavePassword",true);
    values.put("TextZoom",100);
    values.put("AcceptThirdPartyCookies",false);
    values.put("DefaultZoom",ZoomDensity.MEDIUM);
    values.put("LightTouchEnabled",false);
    values.put("UserAgent",0);
    values.put("UseWideViewPort",false);
    values.put("SupportMultipleWindows",false);
    values.put("LayoutAlgorithm",LayoutAlgorithm.NARROW_COLUMNS);
    values.put("StandardFontFamily","sans-serif");
    values.put("FixedFontFamily","monospace");
    values.put("SansSerifFontFamily","sans-serif");
    values.put("SerifFontFamily","serif");
    values.put("CursiveFontFamily","cursive");
    values.put("FantasyFontFamily","fantasy");
    values.put("MinimumFontSize",8);
    values.put("MinimumLogicalFontSize",8);
    values.put("DefaultFontSize",16);
    values.put("DefaultFixedFontSize",13);
    values.put("LoadsImagesAutomatically",true);
    values.put("BlockNetworkImage",false);
    values.put("BlockNetworkLoads",false);
    values.put("JavaScriptEnabled",false);
    values.put("AllowUniversalAccessFromFileURLs",false);
    values.put("AllowFileAccessFromFileURLs",false);
    values.put("PluginsEnabled",false);
    values.put("PluginState",PluginState.OFF);
    values.put("DatabasePath","");
    values.put("GeolocationDatabasePath","");
    values.put("DatabaseEnabled",false);
    values.put("DomStorageEnabled",false);
    values.put("GeolocationEnabled",true);
    values.put("JavaScriptCanOpenWindowsAutomatically",false);
    values.put("DefaultTextEncodingName","UTF-8");
    values.put("UserAgentString",WebKitFactory.userAgent());
    values.put("NeedInitialFocus",true);
    values.put("RenderPriority",RenderPriority.NORMAL);
    values.put("CacheMode",LOAD_DEFAULT);
    values.put("MixedContentMode",MIXED_CONTENT_NEVER_ALLOW);
    values.put("VideoOverlayForEmbeddedEncryptedVideoEnabled",false);
    values.put("OffscreenPreRaster",false);
    values.put("SafeBrowsingEnabled",true);
    values.put("DisabledActionModeMenuItems",0);
    values.forEach((key,value)->changed.accept(key,wire(value)));
  }
  private static Object wire(Object value){return value instanceof Enum<?> e?e.name():value;}
  private synchronized void put(String key,Object value){values.put(key,value);changed.accept(key,wire(value));}
  private synchronized Object get(String key){return values.get(key);}
  @Override public void setNavDump(boolean value){put("NavDump",value);}
  @Override public boolean getNavDump(){return (boolean)get("NavDump");}
  @Override public void setSupportZoom(boolean value){put("SupportZoom",value);}
  @Override public boolean supportZoom(){return (boolean)get("SupportZoom");}
  @Override public void setMediaPlaybackRequiresUserGesture(boolean value){put("MediaPlaybackRequiresUserGesture",value);}
  @Override public boolean getMediaPlaybackRequiresUserGesture(){return (boolean)get("MediaPlaybackRequiresUserGesture");}
  @Override public void setBuiltInZoomControls(boolean value){put("BuiltInZoomControls",value);}
  @Override public boolean getBuiltInZoomControls(){return (boolean)get("BuiltInZoomControls");}
  @Override public void setDisplayZoomControls(boolean value){put("DisplayZoomControls",value);}
  @Override public boolean getDisplayZoomControls(){return (boolean)get("DisplayZoomControls");}
  @Override public void setAllowFileAccess(boolean value){put("AllowFileAccess",value);}
  @Override public boolean getAllowFileAccess(){return (boolean)get("AllowFileAccess");}
  @Override public void setAllowContentAccess(boolean value){put("AllowContentAccess",value);}
  @Override public boolean getAllowContentAccess(){return (boolean)get("AllowContentAccess");}
  @Override public void setLoadWithOverviewMode(boolean value){put("LoadWithOverviewMode",value);}
  @Override public boolean getLoadWithOverviewMode(){return (boolean)get("LoadWithOverviewMode");}
  @Override public void setEnableSmoothTransition(boolean value){put("EnableSmoothTransition",value);}
  @Override public boolean enableSmoothTransition(){return (boolean)get("EnableSmoothTransition");}
  @Override public void setUseWebViewBackgroundForOverscrollBackground(boolean value){put("UseWebViewBackgroundForOverscrollBackground",value);}
  @Override public boolean getUseWebViewBackgroundForOverscrollBackground(){return (boolean)get("UseWebViewBackgroundForOverscrollBackground");}
  @Override public void setSaveFormData(boolean value){put("SaveFormData",value);}
  @Override public boolean getSaveFormData(){return (boolean)get("SaveFormData");}
  @Override public void setSavePassword(boolean value){put("SavePassword",value);}
  @Override public boolean getSavePassword(){return (boolean)get("SavePassword");}
  @Override public void setTextZoom(int value){put("TextZoom",value);}
  @Override public int getTextZoom(){return (int)get("TextZoom");}
  @Override public void setAcceptThirdPartyCookies(boolean value){put("AcceptThirdPartyCookies",value);}
  @Override public boolean getAcceptThirdPartyCookies(){return (boolean)get("AcceptThirdPartyCookies");}
  @Override public void setDefaultZoom(ZoomDensity value){put("DefaultZoom",value);}
  @Override public ZoomDensity getDefaultZoom(){return (ZoomDensity)get("DefaultZoom");}
  @Override public void setLightTouchEnabled(boolean value){put("LightTouchEnabled",value);}
  @Override public boolean getLightTouchEnabled(){return (boolean)get("LightTouchEnabled");}
  @Override public void setUserAgent(int value){put("UserAgent",value);}
  @Override public int getUserAgent(){return (int)get("UserAgent");}
  @Override public void setUseWideViewPort(boolean value){put("UseWideViewPort",value);}
  @Override public boolean getUseWideViewPort(){return (boolean)get("UseWideViewPort");}
  @Override public void setSupportMultipleWindows(boolean value){put("SupportMultipleWindows",value);}
  @Override public boolean supportMultipleWindows(){return (boolean)get("SupportMultipleWindows");}
  @Override public void setLayoutAlgorithm(LayoutAlgorithm value){put("LayoutAlgorithm",value);}
  @Override public LayoutAlgorithm getLayoutAlgorithm(){return (LayoutAlgorithm)get("LayoutAlgorithm");}
  @Override public void setStandardFontFamily(java.lang.String value){put("StandardFontFamily",value);}
  @Override public java.lang.String getStandardFontFamily(){return (java.lang.String)get("StandardFontFamily");}
  @Override public void setFixedFontFamily(java.lang.String value){put("FixedFontFamily",value);}
  @Override public java.lang.String getFixedFontFamily(){return (java.lang.String)get("FixedFontFamily");}
  @Override public void setSansSerifFontFamily(java.lang.String value){put("SansSerifFontFamily",value);}
  @Override public java.lang.String getSansSerifFontFamily(){return (java.lang.String)get("SansSerifFontFamily");}
  @Override public void setSerifFontFamily(java.lang.String value){put("SerifFontFamily",value);}
  @Override public java.lang.String getSerifFontFamily(){return (java.lang.String)get("SerifFontFamily");}
  @Override public void setCursiveFontFamily(java.lang.String value){put("CursiveFontFamily",value);}
  @Override public java.lang.String getCursiveFontFamily(){return (java.lang.String)get("CursiveFontFamily");}
  @Override public void setFantasyFontFamily(java.lang.String value){put("FantasyFontFamily",value);}
  @Override public java.lang.String getFantasyFontFamily(){return (java.lang.String)get("FantasyFontFamily");}
  @Override public void setMinimumFontSize(int value){put("MinimumFontSize",value);}
  @Override public int getMinimumFontSize(){return (int)get("MinimumFontSize");}
  @Override public void setMinimumLogicalFontSize(int value){put("MinimumLogicalFontSize",value);}
  @Override public int getMinimumLogicalFontSize(){return (int)get("MinimumLogicalFontSize");}
  @Override public void setDefaultFontSize(int value){put("DefaultFontSize",value);}
  @Override public int getDefaultFontSize(){return (int)get("DefaultFontSize");}
  @Override public void setDefaultFixedFontSize(int value){put("DefaultFixedFontSize",value);}
  @Override public int getDefaultFixedFontSize(){return (int)get("DefaultFixedFontSize");}
  @Override public void setLoadsImagesAutomatically(boolean value){put("LoadsImagesAutomatically",value);}
  @Override public boolean getLoadsImagesAutomatically(){return (boolean)get("LoadsImagesAutomatically");}
  @Override public void setBlockNetworkImage(boolean value){put("BlockNetworkImage",value);}
  @Override public boolean getBlockNetworkImage(){return (boolean)get("BlockNetworkImage");}
  @Override public void setBlockNetworkLoads(boolean value){put("BlockNetworkLoads",value);}
  @Override public boolean getBlockNetworkLoads(){return (boolean)get("BlockNetworkLoads");}
  @Override public void setJavaScriptEnabled(boolean value){put("JavaScriptEnabled",value);}
  @Override public void setAllowUniversalAccessFromFileURLs(boolean value){put("AllowUniversalAccessFromFileURLs",value);}
  @Override public void setAllowFileAccessFromFileURLs(boolean value){put("AllowFileAccessFromFileURLs",value);}
  @Override public void setPluginsEnabled(boolean value){put("PluginsEnabled",value);}
  @Override public void setPluginState(PluginState value){put("PluginState",value);}
  @Override public void setDatabasePath(java.lang.String value){put("DatabasePath",value);}
  @Override public void setGeolocationDatabasePath(java.lang.String value){put("GeolocationDatabasePath",value);}
  @Override public void setDatabaseEnabled(boolean value){put("DatabaseEnabled",value);}
  @Override public void setDomStorageEnabled(boolean value){put("DomStorageEnabled",value);}
  @Override public boolean getDomStorageEnabled(){return (boolean)get("DomStorageEnabled");}
  @Override public java.lang.String getDatabasePath(){return (java.lang.String)get("DatabasePath");}
  @Override public boolean getDatabaseEnabled(){return (boolean)get("DatabaseEnabled");}
  @Override public void setGeolocationEnabled(boolean value){put("GeolocationEnabled",value);}
  @Override public boolean getJavaScriptEnabled(){return (boolean)get("JavaScriptEnabled");}
  @Override public boolean getAllowUniversalAccessFromFileURLs(){return (boolean)get("AllowUniversalAccessFromFileURLs");}
  @Override public boolean getAllowFileAccessFromFileURLs(){return (boolean)get("AllowFileAccessFromFileURLs");}
  @Override public boolean getPluginsEnabled(){return (boolean)get("PluginsEnabled");}
  @Override public PluginState getPluginState(){return (PluginState)get("PluginState");}
  @Override public void setJavaScriptCanOpenWindowsAutomatically(boolean value){put("JavaScriptCanOpenWindowsAutomatically",value);}
  @Override public boolean getJavaScriptCanOpenWindowsAutomatically(){return (boolean)get("JavaScriptCanOpenWindowsAutomatically");}
  @Override public void setDefaultTextEncodingName(java.lang.String value){put("DefaultTextEncodingName",value);}
  @Override public java.lang.String getDefaultTextEncodingName(){return (java.lang.String)get("DefaultTextEncodingName");}
  @Override public void setUserAgentString(java.lang.String value){put("UserAgentString",value);}
  @Override public java.lang.String getUserAgentString(){return (java.lang.String)get("UserAgentString");}
  @Override public void setNeedInitialFocus(boolean value){put("NeedInitialFocus",value);}
  @Override public void setRenderPriority(RenderPriority value){put("RenderPriority",value);}
  @Override public void setCacheMode(int value){put("CacheMode",value);}
  @Override public int getCacheMode(){return (int)get("CacheMode");}
  @Override public void setMixedContentMode(int value){put("MixedContentMode",value);}
  @Override public int getMixedContentMode(){return (int)get("MixedContentMode");}
  @Override public void setVideoOverlayForEmbeddedEncryptedVideoEnabled(boolean value){put("VideoOverlayForEmbeddedEncryptedVideoEnabled",value);}
  @Override public boolean getVideoOverlayForEmbeddedEncryptedVideoEnabled(){return (boolean)get("VideoOverlayForEmbeddedEncryptedVideoEnabled");}
  @Override public void setOffscreenPreRaster(boolean value){put("OffscreenPreRaster",value);}
  @Override public boolean getOffscreenPreRaster(){return (boolean)get("OffscreenPreRaster");}
  @Override public void setSafeBrowsingEnabled(boolean value){put("SafeBrowsingEnabled",value);}
  @Override public boolean getSafeBrowsingEnabled(){return (boolean)get("SafeBrowsingEnabled");}
  @Override public void setDisabledActionModeMenuItems(int value){put("DisabledActionModeMenuItems",value);}
  @Override public int getDisabledActionModeMenuItems(){return (int)get("DisabledActionModeMenuItems");}
}
