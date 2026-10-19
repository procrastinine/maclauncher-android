#import <WebKit/WebKit.h>
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <CommonCrypto/CommonDigest.h>
#include "webkit-platform.h"
#include <stdexcept>
#include <memory>
#include <unordered_map>
#include <atomic>
#include <vector>
#include <deque>
#include <algorithm>
#include <pthread.h>
#include <climits>
#include <mutex>
#include <cmath>
#include "network-policy.h"
#include "webview-ad-rules.h"


// The Android View tree owns visibility of this image producer. Its carrier
// window can be covered while Android still needs animation and captured frames.
@interface WKWebView (AndroidCaptureActivity)
- (void)_setWindowOcclusionDetectionEnabled:(BOOL)enabled;
- (void)_setTextZoomFactor:(double)factor;
@end

// WebKit's original navigation identity. These selectors are available on the
// supported macOS releases; the same identity is public on newer SDKs.
@interface WKNavigationAction (AndroidNavigationOrigin)
- (WKNavigation*)_mainFrameNavigation;
- (BOOL)_isRedirect;
- (BOOL)_isUserInitiated;
@end

struct KljWebContext {
WKWebsiteDataStore* store=nil;
NSString* privateDirectory=nil;
NSString* appDataRoot=nil;
bool networkAllowed=false;
WKContentRuleList* networkRules=nil;
WKContentRuleList* advertisingRules=nil;
};
std::shared_ptr<KljWebContext> klj_webkit_context(){return std::make_shared<KljWebContext>();}

namespace {
// One capture is in flight per browser. Java consumes the direct pixel view
// before returning. CoreGraphics and the supplied Bitmap use premultiplied BGRA.
struct SnapshotPixels {
    std::vector<uint32_t> bytes;
    CGContextRef context=nullptr;
    int width=0,height=0;
    void clear(){if(context)CGContextRelease(context);context=nullptr;std::vector<uint32_t>().swap(bytes);width=height=0;}
    bool prepare(int w,int h){
        if(context&&w==width&&h==height)return true;
        clear();if(w<=0||h<=0||h>INT_MAX/w)return false;
        try{bytes.resize((size_t)w*h);}catch(const std::bad_alloc&){return false;}
        CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        context=CGBitmapContextCreate(bytes.data(),w,h,8,(size_t)w*4,color,(uint32_t)kCGBitmapByteOrder32Little|(uint32_t)kCGImageAlphaPremultipliedFirst);
        CGColorSpaceRelease(color);
        if(!context){clear();return false;}
        CGContextSetBlendMode(context,kCGBlendModeCopy);width=w;height=h;return true;
    }
};
struct Browser;
struct EditorChange {NSString*before;NSString*after;NSDictionary*metadata;WKFrameInfo*frame;NSString*identity;};
using Ref=std::shared_ptr<Browser>;

std::unordered_map<int64_t,Ref> browsers;
int64_t nextHandle=1;
id keyboardMonitor=nil;
std::mutex configurationMutex;
char navigationObservation;
[[noreturn]] void fail(const char*message){throw std::runtime_error(message);}
[[noreturn]] void fail(const char*message,NSError*error){NSString*detail=error?[NSString stringWithFormat:@"%s: %@ (%ld), %@",message,error.domain,(long)error.code,error.userInfo[NSHelpAnchorErrorKey]?:error.localizedDescription]:@(message);fail(detail.UTF8String);}
NSString* json(id value){if(!value||value==[NSNull null])return @"null";NSData* bytes=[NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingFragmentsAllowed error:nil];return bytes?[[NSString alloc]initWithData:bytes encoding:NSUTF8StringEncoding]:@"null";}
Ref owner(int64_t handle){if(!pthread_main_np())fail("WebKit requires the native window thread");auto found=browsers.find(handle);if(found==browsers.end())fail("WebKit view has been destroyed");return found->second;}
bool localURL(const std::shared_ptr<KljWebContext>&context,NSURL*url){if(!url.isFileURL)return true;NSString*path=url.URLByResolvingSymlinksInPath.path;return [path hasPrefix:[context->appDataRoot stringByAppendingString:@"/"]]||[path isEqualToString:context->appDataRoot];}
bool advertisingURL(NSURL*url){return url.host&&!klj_network_host_allowed(url.host.UTF8String);}
NSString* androidURL(NSURL*url){NSString*scheme=url.scheme.lowercaseString;NSString*root=[scheme isEqualToString:@"android-asset"]?@"file:///android_asset":[scheme isEqualToString:@"android-resource"]?@"file:///android_res":nil;if(!root)return url.absoluteString;NSURLComponents*parts=[NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];return [NSString stringWithFormat:@"%@%@%@%@",root,parts.percentEncodedPath?:@"/",parts.percentEncodedQuery?[@"?" stringByAppendingString:parts.percentEncodedQuery]:@"",parts.percentEncodedFragment?[@"#" stringByAppendingString:parts.percentEncodedFragment]:@""];}
NSURL* resourceURL(NSString*text){if([text hasPrefix:@"file:///android_asset/"])text=[@"android-asset:///" stringByAppendingString:[text substringFromIndex:22]];else if([text hasPrefix:@"file:///android_res/"])text=[@"android-resource:///" stringByAppendingString:[text substringFromIndex:20]];return [NSURL URLWithString:text];}
}

@interface MLAndroidWebDelegate : NSObject<WKNavigationDelegate,WKUIDelegate,WKScriptMessageHandler,WKURLSchemeHandler>
@property(nonatomic) std::weak_ptr<Browser> owner;
@end
// Native mouse dispatch belongs to the existing SDL window. Android dispatches
// events to this invisible rendering view explicitly after normal View routing.
@interface MLAndroidWebView : WKWebView
- (NSView*)androidTargetAtPoint:(NSPoint)point;
@end
@implementation MLAndroidWebView
- (NSView*)hitTest:(NSPoint)point {return nil;}
- (NSView*)androidTargetAtPoint:(NSPoint)point {return [super hitTest:point];}
@end


namespace {
struct Browser {
    WKWebView* view=nil;WKFrameInfo* editorFrame=nil;NSString* editorIdentity=nil;__weak NSWindow* window=nil;__weak NSResponder* inputResponder=nil;__weak NSResponder* nativeInputResponder=nil;bool visible=false,focused=false;MLAndroidWebDelegate* delegate=nil;
    NSMutableDictionary<NSNumber*,void(^)(BOOL,NSString*)>* dialogs=[NSMutableDictionary new];
    NSArray<WKUserScript*>* baseScripts=nil;
    NSArray<NSString*>* interfaceNames=@[];
    NSHashTable<WKNavigation*>* applicationNavigations=[NSHashTable weakObjectsHashTable];
    int64_t nextDialog=1;
    std::shared_ptr<KljWebClient> client;std::shared_ptr<KljWebContext> context;
    bool closed=false,snapshot=false,editorPending=false;
    std::deque<EditorChange> editorChanges;
    SnapshotPixels pixels;
    int pixelWidth=1,pixelHeight=1,contentWidth=0,contentHeight=0;CGFloat density=1;
    Browser(std::shared_ptr<KljWebContext>c,std::shared_ptr<KljWebClient> callback):client(std::move(callback)),context(std::move(c)){}
    void applicationLoad(WKNavigation* navigation){if(navigation)[applicationNavigations addObject:navigation];}
    bool startsApplicationLoad(WKNavigationAction* action){
        if(!action.targetFrame.mainFrame)return false;
        WKNavigation* navigation;
        if(@available(macOS 27.0,*))navigation=action.mainFrameNavigation;
        else navigation=[action _mainFrameNavigation];
        if(!navigation||![applicationNavigations containsObject:navigation])return false;
        [applicationNavigations removeObject:navigation];
        return ![action _isRedirect];
    }
    void close(){if(closed)return;setFocus(false);closed=true;editorChanges.clear();for(void(^done)(BOOL,NSString*) in dialogs.allValues)done(NO,nil);[dialogs removeAllObjects];[view stopLoading];[view removeObserver:delegate forKeyPath:@"URL" context:&navigationObservation];view.navigationDelegate=nil;view.UIDelegate=nil;[view.configuration.userContentController removeAllScriptMessageHandlers];[view removeFromSuperview];window=nil;inputResponder=nil;nativeInputResponder=nil;editorFrame=nil;editorIdentity=nil;view=nil;delegate=nil;pixels.clear();client.reset();}
    ~Browser(){close();}
    bool ownsResponder(NSResponder*responder){return [responder isKindOfClass:NSView.class]&&[(NSView*)responder isDescendantOf:view];}
    void retainEditor(){NSResponder*responder=window.firstResponder;if(ownsResponder(responder))inputResponder=responder;else if(responder)nativeInputResponder=responder;if(focused&&visible&&window&&!ownsResponder(window.firstResponder)){[window makeFirstResponder:view];if(ownsResponder(window.firstResponder))inputResponder=window.firstResponder;}}
    void setFocus(bool value){focused=value;if(value)retainEditor();else if(ownsResponder(window.firstResponder))[window makeFirstResponder:nativeInputResponder?:view.superview];}
    void bind(){NSView*parent=client?(__bridge NSView*)client->contentView():nil;if(parent&&view.superview!=parent){[view removeFromSuperview];[parent addSubview:view positioned:NSWindowBelow relativeTo:parent.subviews.firstObject];view.wantsLayer=YES;view.layer.opacity=0;window=parent.window;}view.hidden=!visible;}
    void dirty(){if(!closed)client->changed();}
    void dimensions(NSDictionary*value){
        if(closed)return;
        auto extent=[](id number)->int{double value=[number isKindOfClass:NSNumber.class]?[number doubleValue]:0;return std::isfinite(value)?(int)std::clamp(value,0.,(double)INT_MAX):0;};
        int width=extent(value[@"width"]),height=extent(value[@"height"]);
        if(width==contentWidth&&height==contentHeight)return;
        contentWidth=width;contentHeight=height;
        client->dimensions(width,height);
    }
    void navigation(NSString*state,NSString*error=nil){if(closed)return;client->navigation(state,androidURL(view.URL),error?:view.title,(int)lround(view.estimatedProgress*100));if(closed)return;auto list=view.backForwardList;NSArray<WKBackForwardListItem*>*items=list.currentItem?[[list.backList arrayByAddingObject:list.currentItem]arrayByAddingObjectsFromArray:list.forwardList]:@[];NSMutableArray*urls=[NSMutableArray new],*titles=[NSMutableArray new];for(WKBackForwardListItem*item in items){[urls addObject:androidURL(item.URL)?:@""];[titles addObject:item.title?:@""];}client->history(urls,titles,list.currentItem?(int)list.backList.count:-1);}
    void dialog(NSString*kind,NSString*message,NSString*value,void(^completion)(BOOL,NSString*)){if(closed){completion(NO,nil);return;}int64_t id=nextDialog++;dialogs[@(id)]=completion;client->dialog(id,kind,androidURL(view.URL),message,value);}
};
struct Completion {
    std::shared_ptr<KljWebResult> callback;
    explicit Completion(std::shared_ptr<KljWebResult> value):callback(std::move(value)){}
    void done(id value,NSString*error=nil){auto result=std::move(callback);if(result)result->complete(json(value),error);}
};
// CookieManager is synchronous. Read its actual WebKit owner at the call
// boundary; an observer cache can lag behind document.cookie or a response.
NSArray<NSHTTPCookie*>* currentCookies(const std::shared_ptr<KljWebContext>&context){
    __block NSArray<NSHTTPCookie*>* result=@[];
    dispatch_semaphore_t ready=dispatch_semaphore_create(0);
    void(^read)(void)=^{
        if(!context->store){dispatch_semaphore_signal(ready);return;}
        [context->store.httpCookieStore getAllCookies:^(NSArray<NSHTTPCookie*>*cookies){result=cookies;dispatch_semaphore_signal(ready);}];
    };
    if(pthread_main_np()){
        read();
        while(dispatch_semaphore_wait(ready,DISPATCH_TIME_NOW))
            [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:.005]];
    }else{dispatch_async(dispatch_get_main_queue(),read);dispatch_semaphore_wait(ready,DISPATCH_TIME_FOREVER);}
    return result;
}
void refreshCookies(std::shared_ptr<KljWebContext> context,void(^done)(void)){[context->store.httpCookieStore getAllCookies:^(NSArray<NSHTTPCookie*>*cookies){if(done)done();}];}
// Each frame reports its own editor state. Only an actual focus transition
// transfers ownership; unrelated frame loads and mutations cannot retire it.
NSString* editorScript=@"(()=>{ const identity=Array.from(crypto.getRandomValues(new Uint32Array(4)),x=>x.toString(16)).join('-'),elements=new WeakMap();let nextElement=0,paused=false,composingStart=-1,composingEnd=-1; const element=()=>{const e=document.activeElement;if(e&&!elements.has(e))elements.set(e,++nextElement);return e?elements.get(e):0;}; const content=()=>{const e=document.activeElement;return e?.value??e?.textContent??'';}; const position=(node,offset)=>{const e=document.activeElement;if(!e?.contains(node))return 0;const r=document.createRange();r.selectNodeContents(e);r.setEnd(node,offset);return r.toString().length;}; const selection=()=>{const e=document.activeElement,s=getSelection();return typeof e?.selectionStart==='number'?[e.selectionStart,e.selectionEnd]:s&&e?.contains(s.anchorNode)&&e.contains(s.focusNode)?[position(s.anchorNode,s.anchorOffset),position(s.focusNode,s.focusOffset)]:[0,0];}; const select=(start,end)=>{const e=document.activeElement;if(typeof e?.setSelectionRange==='function'){e.setSelectionRange(Math.min(start,end),Math.max(start,end),start>end?'backward':'forward');return;}if(!e?.isContentEditable)return;const point=offset=>{const w=document.createTreeWalker(e,NodeFilter.SHOW_TEXT);let n,last;while(n=w.nextNode()){last=n;if(offset<=n.length)return[n,offset];offset-=n.length;}return last?[last,last.length]:[e,0];},a=point(start),b=point(end);getSelection().setBaseAndExtent(a[0],a[1],b[0],b[1]);}; const notify=kind=>{if(paused)return;const e=document.activeElement,editable=!!e&&(e.tagName==='TEXTAREA'||e.tagName==='INPUT'&&!['button','submit','reset','checkbox','radio','range','color','file','image','hidden'].includes(e.type)||e.isContentEditable),text=editable?content():'',[start,end]=selection();window.webkit.messageHandlers.maclauncherEditor.postMessage({identity,element:element(),revision:window.__maclauncherEditorRevision||0,kind,editable,password:e?.type==='password',text,start,end,composingStart,composingEnd});}; window.__maclauncherEditorBegin=(revision,token)=>{if(identity+':'+element()!==token)return false;paused=true;window.__maclauncherEditorRevision=revision;return true;}; window.__maclauncherEditorEnd=(start,end,from,to)=>{try{composingStart=from;composingEnd=to;select(start,end);}finally{paused=false;notify('android');}}; for(const event of ['focusin','focusout','input','selectionchange'])addEventListener(event,()=>{if(event==='focusin'||event==='focusout'){composingStart=composingEnd=-1;window.__maclauncherEditorRevision=0;}notify(event);},true); new MutationObserver(()=>notify('mutation')).observe(document,{childList:true,subtree:true,attributes:true,attributeFilter:['type','readonly','disabled','contenteditable']});notify('initial');})();";
NSString* observerScript=@"(()=>{const changed=()=>{const d=document.documentElement,b=document.body;window.webkit.messageHandlers.maclauncherChanged.postMessage(window===top?{width:Math.max(d?.scrollWidth||0,b?.scrollWidth||0,d?.clientWidth||0),height:Math.max(d?.scrollHeight||0,b?.scrollHeight||0,d?.clientHeight||0)}:0);},raf=requestAnimationFrame.bind(window);let scheduled=false,animating=false;const notify=()=>{if(scheduled)return;scheduled=true;queueMicrotask(()=>{scheduled=false;changed();});};const animate=()=>{if(animating)return;animating=true;raf(()=>{animating=false;notify();if(document.getAnimations().some(a=>a.playState==='running'))animate();});};window.requestAnimationFrame=callback=>raf(time=>{try{callback(time);}finally{notify();}});new MutationObserver(notify).observe(document,{subtree:true,attributes:true,characterData:true,childList:true});const sizes=new ResizeObserver(notify);sizes.observe(document.documentElement);if(document.body)sizes.observe(document.body);for(const e of ['load','scroll','resize','input','playing','timeupdate'])addEventListener(e,notify,true);for(const e of ['animationstart','transitionstart'])addEventListener(e,()=>{notify();animate();},true);notify();})();";
}

@implementation MLAndroidWebDelegate
- (void)observeValueForKeyPath:(NSString*)keyPath ofObject:(id)object change:(NSDictionary*)change context:(void*)context {if(context==&navigationObservation){if(auto s=self.owner.lock())if(!s->closed&&s->view.URL)s->navigation(@"history");return;}[super observeValueForKeyPath:keyPath ofObject:object change:change context:context];}
// Android location is supplied manually through its location service. Native
// WebKit must never start CoreLocation or select a Mac capture device itself.
- (void)webView:(WKWebView*)view requestGeolocationPermissionForOrigin:(WKSecurityOrigin*)origin initiatedByFrame:(WKFrameInfo*)frame decisionHandler:(void(^)(WKPermissionDecision))decision {decision(WKPermissionDecisionDeny);}
// These are the corresponding WebKit callbacks on supported pre-27 systems.
- (void)_webView:(WKWebView*)view requestGeolocationPermissionForOrigin:(WKSecurityOrigin*)origin initiatedByFrame:(WKFrameInfo*)frame decisionHandler:(void(^)(WKPermissionDecision))decision {decision(WKPermissionDecisionDeny);}
- (void)_webView:(WKWebView*)view requestGeolocationPermissionForFrame:(WKFrameInfo*)frame decisionHandler:(void(^)(BOOL))decision {decision(NO);}
- (void)webView:(WKWebView*)view requestMediaCapturePermissionForOrigin:(WKSecurityOrigin*)origin initiatedByFrame:(WKFrameInfo*)frame type:(WKMediaCaptureType)type decisionHandler:(void(^)(WKPermissionDecision))decision {decision(WKPermissionDecisionDeny);}
- (void)webView:(WKWebView*)view startURLSchemeTask:(id<WKURLSchemeTask>)task {
    auto s=self.owner.lock();NSData*bytes=s&&!s->closed?s->client->resource(task.request.URL.absoluteString):nil;if(!bytes){[task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorFileDoesNotExist userInfo:nil]];return;}NSString*mime=[UTType typeWithFilenameExtension:task.request.URL.pathExtension].preferredMIMEType?:@"application/octet-stream";[task didReceiveResponse:[[NSURLResponse alloc]initWithURL:task.request.URL MIMEType:mime expectedContentLength:bytes.length textEncodingName:@"utf-8"]];[task didReceiveData:bytes];[task didFinish];
}
- (void)webView:(WKWebView*)view stopURLSchemeTask:(id<WKURLSchemeTask>)task {}
- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {if(auto s=self.owner.lock()){if([message.name isEqualToString:@"maclauncherNavigation"]){if(message.frameInfo.mainFrame){s->navigation(@"finished");s->dirty();}}else if([message.name isEqualToString:@"maclauncherChanged"]){if(message.frameInfo.mainFrame&&[message.body isKindOfClass:NSDictionary.class])s->dimensions(message.body);s->dirty();}else if([message.name isEqualToString:@"maclauncherEditor"]&&[message.body isKindOfClass:NSDictionary.class]){NSString*identity=message.body[@"identity"];bool same=[identity isKindOfClass:NSString.class]&&[identity isEqual:s->editorIdentity];bool focus=[message.body[@"kind"]isEqual:@"focusin"];if(![identity isKindOfClass:NSString.class]||(!same&&!focus&&(s->editorIdentity||![message.body[@"kind"]isEqual:@"initial"])))return;
        if(!s->closed){s->retainEditor();bool editable=[message.body[@"editable"]boolValue];s->editorFrame=editable?message.frameInfo:nil;s->editorIdentity=editable?identity:nil;s->client->editor(json(message.body));}s->retainEditor();s->dirty();}else if([message.name isEqualToString:@"maclauncherPorts"]&&[message.body isKindOfClass:NSDictionary.class]&&[message.body[@"id"] isKindOfClass:NSNumber.class]&&[message.body[@"data"] isKindOfClass:NSString.class]){if(!s->closed)s->client->port([message.body[@"id"]longLongValue],message.body[@"data"]);}else s->dirty();}}
- (void)webView:(WKWebView*)view didStartProvisionalNavigation:(WKNavigation*)navigation {if(auto s=self.owner.lock()){s->editorIdentity=nil;s->editorFrame=nil;s->dimensions(@{});s->navigation(@"started");}}
- (void)webView:(WKWebView*)view didFinishNavigation:(WKNavigation*)navigation {if(auto s=self.owner.lock()){s->navigation(@"finished");s->dirty();}}
- (void)webView:(WKWebView*)view didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {if(auto s=self.owner.lock())s->navigation(@"error",error.localizedDescription);}
- (void)webView:(WKWebView*)view didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {if(auto s=self.owner.lock())s->navigation(@"error",error.localizedDescription);}
- (void)webView:(WKWebView*)view decidePolicyForNavigationAction:(WKNavigationAction*)action decisionHandler:(void (^)(WKNavigationActionPolicy))decide {
    auto s=self.owner.lock();if(!s||s->closed){decide(WKNavigationActionPolicyCancel);return;}BOOL cancel=NO;
    // WK content rules reject actual network requests, including navigation.
    // A base URL on an inline document is an origin, not a network fetch.
    cancel=!localURL(s->context,action.request.URL)||advertisingURL(action.request.URL);
    // Android does not ask a client to override its own loadUrl/postUrl call.
    // Consume only that original navigation's initial action; page navigation
    // and HTTP redirects retain the normal client callback and policy checks.
    bool application=s->startsApplicationLoad(action);
    if(!cancel&&!application&&![action.request.HTTPMethod isEqualToString:@"POST"])cancel=s->client->navigationRequest(androidURL(action.request.URL),action.targetFrame.mainFrame,[action _isUserInitiated],[action _isRedirect],action.request.HTTPMethod?:@"GET",json(action.request.allHTTPHeaderFields?:@{}));
    decide(cancel||s->closed?WKNavigationActionPolicyCancel:WKNavigationActionPolicyAllow);
}
- (void)webView:(WKWebView*)view runJavaScriptAlertPanelWithMessage:(NSString*)message initiatedByFrame:(WKFrameInfo*)frame completionHandler:(void(^)(void))completion {if(auto s=self.owner.lock())s->dialog(@"alert",message,nil,^(BOOL yes,NSString*value){completion();});else completion();}
- (void)webView:(WKWebView*)view runJavaScriptConfirmPanelWithMessage:(NSString*)message initiatedByFrame:(WKFrameInfo*)frame completionHandler:(void(^)(BOOL))completion {if(auto s=self.owner.lock())s->dialog(@"confirm",message,nil,^(BOOL yes,NSString*value){completion(yes);});else completion(NO);}
- (void)webView:(WKWebView*)view runJavaScriptTextInputPanelWithPrompt:(NSString*)prompt defaultText:(NSString*)text initiatedByFrame:(WKFrameInfo*)frame completionHandler:(void(^)(NSString*))completion {
    auto s=self.owner.lock();if(!s){completion(nil);return;}if([prompt isEqualToString:@"__MACLAUNCHER_JAVA__"]){NSDictionary*call=[NSJSONSerialization JSONObjectWithData:[text dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];if(![call isKindOfClass:NSDictionary.class]||![call[@"name"] isKindOfClass:NSString.class]||![call[@"method"] isKindOfClass:NSString.class]||![call[@"args"] isKindOfClass:NSArray.class]){completion(@"{\"ok\":false,\"error\":\"Invalid JavaScript interface request\"}");return;}completion(s->client->interface(call[@"name"],call[@"method"],json(call[@"args"])));return;}s->dialog(@"prompt",prompt,text,^(BOOL yes,NSString*value){completion(yes?value:nil);});
}
@end

static bool ensureStore(std::shared_ptr<KljWebContext> context){if(!pthread_main_np()||!context->privateDirectory){fail("WebKit storage requires the configured native window thread");return false;}
    [NSApplication sharedApplication];if(!context->store)context->store=WKWebsiteDataStore.nonPersistentDataStore;
    if(!context->networkAllowed&&!context->networkRules){NSURL*path=[NSURL fileURLWithPath:[context->privateDirectory stringByAppendingPathComponent:@"cache/webkit-rules"] isDirectory:YES];[[NSFileManager defaultManager]createDirectoryAtURL:path withIntermediateDirectories:YES attributes:nil error:nil];__block bool done=false;__block NSError*failure=nil;[[WKContentRuleListStore storeWithURL:path]compileContentRuleListForIdentifier:@"android-network-isolation" encodedContentRuleList:@"[{\"trigger\":{\"url-filter\":\"^https?:\"},\"action\":{\"type\":\"block\"}},{\"trigger\":{\"url-filter\":\"^wss?:\"},\"action\":{\"type\":\"block\"}},{\"trigger\":{\"url-filter\":\"^ftp:\"},\"action\":{\"type\":\"block\"}}]" completionHandler:^(WKContentRuleList*rules,NSError*error){context->networkRules=rules;failure=error;done=true;}];while(!done)[NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:.01]];if(failure||!context->networkRules){fail("WebKit network isolation rules could not be installed",failure);return false;}}
    if(!klj_network_ads_allowed()&&!context->advertisingRules){
        NSURL*path=[NSURL fileURLWithPath:[context->privateDirectory stringByAppendingPathComponent:@"cache/webkit-rules"] isDirectory:YES];
        [[NSFileManager defaultManager]createDirectoryAtURL:path withIntermediateDirectories:YES attributes:nil error:nil];
        auto rulesStore=[WKContentRuleListStore storeWithURL:path];
        NSString*encoded=json(klj_webview_ad_rules());NSData*bytes=[encoded dataUsingEncoding:NSUTF8StringEncoding];
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes.bytes,(CC_LONG)bytes.length,digest);
        NSMutableString*identity=[NSMutableString stringWithString:@"android-ad-hosts-"];for(unsigned char byte:digest)[identity appendFormat:@"%02x",byte];
        __block bool done=false;__block NSError*failure=nil;
        [rulesStore lookUpContentRuleListForIdentifier:identity completionHandler:^(WKContentRuleList*cached,NSError*error){
            if(cached){context->advertisingRules=cached;done=true;return;}
            [rulesStore compileContentRuleListForIdentifier:identity encodedContentRuleList:encoded completionHandler:^(WKContentRuleList*compiled,NSError*error){context->advertisingRules=compiled;failure=error;done=true;}];
        }];
        while(!done)[NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
        if(failure||!context->advertisingRules){fail("WebKit advertising rules could not be installed",failure);return false;}
        [rulesStore getAvailableContentRuleListIdentifiers:^(NSArray<NSString*>*identifiers){for(NSString*old in identifiers)if([old hasPrefix:@"android-ad-hosts-"]&&![old isEqualToString:identity])[rulesStore removeContentRuleListForIdentifier:old completionHandler:^(NSError*error){}];}];
    }

    return true;
}
NSString* klj_webkit_initialize(std::shared_ptr<KljWebContext> context,NSString* directory,NSString* files,bool allowed){@autoreleasepool{
    std::lock_guard<std::mutex>lock(configurationMutex);NSString*next=directory.stringByStandardizingPath;
    NSString*scope=files.stringByStandardizingPath;
    if(context->privateDirectory&&(![context->privateDirectory isEqualToString:next]||![context->appDataRoot isEqualToString:scope]||context->networkAllowed!=bool(allowed))){fail("WebKit application scope and network policy cannot change during a session");return nullptr;}
    if(!context->privateDirectory){context->privateDirectory=next;context->appDataRoot=scope;context->networkAllowed=allowed;}
    NSBundle*bundle=[NSBundle bundleForClass:WKWebView.class];return [bundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"]?:[bundle objectForInfoDictionaryKey:@"CFBundleVersion"];
}}
int64_t klj_webkit_create(std::shared_ptr<KljWebContext> context,std::shared_ptr<KljWebClient> callback){@autoreleasepool{
    if(!ensureStore(context))return 0;auto s=std::make_shared<Browser>(context,std::move(callback));
    auto config=[WKWebViewConfiguration new];config.websiteDataStore=context->store;s->delegate=[MLAndroidWebDelegate new];s->delegate.owner=s;
    [config setURLSchemeHandler:s->delegate forURLScheme:@"android-asset"];[config setURLSchemeHandler:s->delegate forURLScheme:@"android-resource"];
    if(context->networkRules)[config.userContentController addContentRuleList:context->networkRules];
    if(context->advertisingRules&&!klj_network_ads_allowed())[config.userContentController addContentRuleList:context->advertisingRules];
    // Actual network protocols are blocked by the content rules and process
    // sandbox. A scheme allowlist in frame-src would also suppress local
    // application messages before Android's navigation client sees them.
    if(!context->networkAllowed){NSString*policy=@"(()=>{for(const key of ['RTCPeerConnection','webkitRTCPeerConnection'])if(key in window)Object.defineProperty(window,key,{value:function(){throw new DOMException('Network access is disabled','SecurityError');},writable:false,configurable:false});})();";[config.userContentController addUserScript:[[WKUserScript alloc]initWithSource:policy injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:NO]];}
    [config.userContentController addScriptMessageHandler:s->delegate name:@"maclauncherChanged"];
    [config.userContentController addScriptMessageHandler:s->delegate name:@"maclauncherPorts"];
    [config.userContentController addScriptMessageHandler:s->delegate name:@"maclauncherEditor"];
    [config.userContentController addScriptMessageHandler:s->delegate name:@"maclauncherNavigation"];
    [config.userContentController addUserScript:[[WKUserScript alloc]initWithSource:@"addEventListener('hashchange',()=>window.webkit.messageHandlers.maclauncherNavigation.postMessage(0));" injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
    [config.userContentController addUserScript:[[WKUserScript alloc]initWithSource:editorScript injectionTime:WKUserScriptInjectionTimeAtDocumentEnd forMainFrameOnly:NO]];
    [config.userContentController addUserScript:[[WKUserScript alloc]initWithSource:observerScript injectionTime:WKUserScriptInjectionTimeAtDocumentEnd forMainFrameOnly:NO]];
    // WKNSArray.copy remains a live controller view; materialize the elements.
    s->baseScripts=[NSArray arrayWithArray:config.userContentController.userScripts];
    s->view=[[MLAndroidWebView alloc]initWithFrame:NSMakeRect(0,0,1,1) configuration:config];s->view.navigationDelegate=s->delegate;s->view.UIDelegate=s->delegate;
    [s->view _setWindowOcclusionDetectionEnabled:NO];
    [s->view addObserver:s->delegate forKeyPath:@"URL" options:NSKeyValueObservingOptionNew context:&navigationObservation];
    s->bind();
    // SDL translates native keys and IME before NSApplication dispatch. Stop
    // the second AppKit delivery while WebKit owns logical editor focus.
    if(!keyboardMonitor)keyboardMonitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown|NSEventMaskKeyUp handler:^NSEvent*(NSEvent*event){for(auto&entry:browsers)if(entry.second->focused&&entry.second->window==event.window){entry.second->retainEditor();if(event.type==NSEventTypeKeyDown&&(event.modifierFlags&NSEventModifierFlagCommand)&&![@[@"c",@"v",@"a",@"x"]containsObject:event.charactersIgnoringModifiers.lowercaseString])[NSApp.mainMenu performKeyEquivalent:event];return nil;}return event;}];
    int64_t handle=nextHandle++;browsers.emplace(handle,s);return handle;
}}
void klj_webkit_destroy(int64_t handle){auto s=owner(handle);if(!s)return;s->close();browsers.erase(handle);if(browsers.empty()&&keyboardMonitor){[NSEvent removeMonitor:keyboardMonitor];keyboardMonitor=nil;}}
void klj_webkit_retire(const std::shared_ptr<KljWebContext>&context){std::vector<int64_t>owned;for(const auto&entry:browsers)if(entry.second->context==context)owned.push_back(entry.first);for(auto handle:owned)klj_webkit_destroy(handle);}
bool klj_webkit_owns(const std::shared_ptr<KljWebContext>&context,int64_t handle){auto found=browsers.find(handle);return found!=browsers.end()&&found->second->context==context;}
void klj_webkit_focus(int64_t handle,bool focused){auto s=owner(handle);if(s){s->setFocus(focused);s->dirty();}}
void klj_webkit_size(int64_t handle,int width,int height,float density){auto s=owner(handle);if(s&&width>0&&height>0&&std::isfinite(density)&&density>0){s->pixelWidth=width;s->pixelHeight=height;s->density=density;s->view.frame=NSMakeRect(0,0,width/density,height/density);s->bind();}}
void klj_webkit_visibility(int64_t handle,bool visible){auto s=owner(handle);if(s){s->visible=visible;s->bind();if(visible)s->dirty();}}
extern "C" void klj_webkit_window_detach(void*window){for(auto&entry:browsers){auto s=entry.second;if(s->window==(__bridge NSWindow*)window){[s->view removeFromSuperview];s->window=nil;}}}
void klj_webkit_load(int64_t handle,NSString* address,NSArray<NSString*>*keys,NSArray<NSString*>*values,NSData*data){@autoreleasepool{
    auto s=owner(handle);if(!s)return;NSURL*url=resourceURL(address);if(!url){fail("WebView URL is invalid");return;}if(!localURL(s->context,url)){fail("WebView file is outside Android application storage");return;}
    if(advertisingURL(url)){s->applicationLoad([s->view loadHTMLString:@"" baseURL:[NSURL URLWithString:@"about:blank"]]);return;}
    auto request=[NSMutableURLRequest requestWithURL:url];for(NSUInteger i=0;i<keys.count;i++)[request setValue:values[i] forHTTPHeaderField:keys[i]];
    if(data){request.HTTPMethod=@"POST";request.HTTPBody=data;}
    s->applicationLoad(url.isFileURL?[s->view loadFileURL:url allowingReadAccessToURL:[NSURL fileURLWithPath:s->context->appDataRoot isDirectory:YES]]:[s->view loadRequest:request]);
}}
void klj_webkit_html(int64_t handle,NSString* html,NSString*base){auto s=owner(handle);NSURL*url=resourceURL(base?:@"about:blank");if(!localURL(s->context,url)){fail("WebView base URL is outside Android application storage");return;}if(s)s->applicationLoad([s->view loadHTMLString:html baseURL:url]);}
void klj_webkit_script(int64_t handle,NSString*script,int64_t request){auto s=owner(handle);if(!s)return;[s->view evaluateJavaScript:script completionHandler:^(id value,NSError*error){if(!s->closed)s->client->script(request,json(value),error.localizedDescription);}];}
void klj_webkit_stop(int64_t handle){auto s=owner(handle);if(s)[s->view stopLoading];}
void klj_webkit_reload(int64_t handle){auto s=owner(handle);if(s)s->applicationLoad([s->view reload]);}
void klj_webkit_navigate(int64_t handle,int delta){auto s=owner(handle);if(!s)return;auto item=[s->view.backForwardList itemAtIndex:delta];if(item)s->applicationLoad([s->view goToBackForwardListItem:item]);}
void klj_webkit_interfaces(int64_t handle,NSString*encoded){auto s=owner(handle);if(!s)return;NSDictionary*spec=[NSJSONSerialization JSONObjectWithData:[encoded dataUsingEncoding:NSUTF8StringEncoding]options:0 error:nil];if(![spec isKindOfClass:NSDictionary.class]){fail("Invalid JavaScript interface description");return;}NSString*source=[NSString stringWithFormat:@"(()=>{for(const name of %@)delete window[name];const spec=%@;for(const [name,methods]of Object.entries(spec)){const target={};for(const method of methods)target[method]=(...args)=>{const response=JSON.parse(prompt('__MACLAUNCHER_JAVA__',JSON.stringify({name,method,args})));if(!response.ok)throw new Error(response.error);return response.value;};window[name]=target;}})();",json(s->interfaceNames),json(spec)];s->interfaceNames=spec.allKeys;auto controller=s->view.configuration.userContentController;[controller removeAllUserScripts];for(WKUserScript*script in s->baseScripts)[controller addUserScript:script];[controller addUserScript:[[WKUserScript alloc]initWithSource:source injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:NO]];[s->view evaluateJavaScript:source completionHandler:nil];}
void klj_webkit_dialogResult(int64_t handle,int64_t request,bool accepted,NSString*value){auto s=owner(handle);if(!s)return;void(^done)(BOOL,NSString*)=s->dialogs[@(request)];if(done){[s->dialogs removeObjectForKey:@(request)];done(accepted,value);}}
NSData* klj_webkit_state(int64_t handle){auto s=owner(handle);if(!s)return nullptr;NSError*error=nil;NSData*data=[NSKeyedArchiver archivedDataWithRootObject:s->view.interactionState requiringSecureCoding:YES error:&error];if(!data||data.length>INT_MAX){fprintf(stderr,"[android-webkit] Session state could not be archived: %s\n",error.localizedDescription.UTF8String?:"invalid size");return nullptr;}return data;}
bool klj_webkit_restore(int64_t handle,NSData*encoded){auto s=owner(handle);if(!s||!encoded)return false;NSData*data=encoded;NSError*error=nil;Class type=[s->view.interactionState class];id value=type?[NSKeyedUnarchiver unarchivedObjectOfClass:type fromData:data error:&error]:nil;if(!value){fprintf(stderr,"[android-webkit] Session state could not be restored: %s\n",error.localizedDescription.UTF8String?:"no initial state");return false;}s->view.interactionState=value;s->navigation(@"restored");s->dirty();return true;}
void klj_webkit_command(int64_t handle,NSString*operation,NSString*encoded,int64_t request){auto s=owner(handle);if(!s)return;NSString*op=operation;NSDictionary*body=[NSJSONSerialization JSONObjectWithData:[encoded dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];auto done=^(id value,NSString*error){if(!s->closed)s->client->script(request,json(value),error);};
    if([op isEqualToString:@"clearCache"]){NSMutableSet*types=[NSMutableSet setWithObject:WKWebsiteDataTypeMemoryCache];if([body[@"disk"]boolValue])[types addObjectsFromArray:@[WKWebsiteDataTypeDiskCache,WKWebsiteDataTypeFetchCache]];[s->context->store removeDataOfTypes:types modifiedSince:NSDate.distantPast completionHandler:^{done(@YES,nil);}];return;}
    if([op isEqualToString:@"clearCredentials"]){done(nil,@"No WebKit certificate exceptions are retained by this provider");return;}
    if([op isEqualToString:@"pause"]){BOOL paused=[body[@"paused"]boolValue];[s->view setAllMediaPlaybackSuspended:paused completionHandler:nil];[s->view evaluateJavaScript:paused?@"document.getAnimations().forEach(a=>a.pause())":@"document.getAnimations().forEach(a=>a.play())" completionHandler:^(id value,NSError*error){done(nil,error.localizedDescription);}];return;}
    if([op isEqualToString:@"find"]){auto config=[WKFindConfiguration new];config.backwards=[body[@"backwards"]boolValue];config.wraps=YES;[s->view findString:body[@"text"]?:@"" withConfiguration:config completionHandler:^(WKFindResult*result){done(@(result.matchFound),nil);s->dirty();}];return;}
    if([op isEqualToString:@"archive"]){NSURL*destination=[NSURL fileURLWithPath:body[@"path"]?:@""];if(!localURL(s->context,destination)){done(nil,@"Web archive destination is outside Android application storage");return;}[s->view createWebArchiveDataWithCompletionHandler:^(NSData*data,NSError*error){NSError*writeError=error;if(data&&!error)[data writeToURL:destination options:NSDataWritingAtomic error:&writeError];done(writeError?nil:destination.path,writeError.localizedDescription);}];return;}
    if([op hasPrefix:@"port"]){NSString*source=[NSString stringWithFormat:@"(()=>{const p=window.__maclauncherPorts||(window.__maclauncherPorts=new Map()),a=%@;",json(body)];if([op isEqualToString:@"portCreate"])source=[source stringByAppendingString:@"const c=new MessageChannel();for(const [id,port]of [[a.first,c.port1],[a.second,c.port2]]){p.set(id,port);port.onmessage=e=>window.webkit.messageHandlers.maclauncherPorts.postMessage({id,data:String(e.data)});port.start();}"];else if([op isEqualToString:@"portClose"])source=[source stringByAppendingString:@"p.get(a.id)?.close();p.delete(a.id);"];else if([op isEqualToString:@"portPost"])source=[source stringByAppendingString:@"p.get(a.id)?.postMessage(a.data,a.ports.map(id=>p.get(id)));for(const id of a.ports)p.delete(id);"];else if([op isEqualToString:@"portTransfer"])source=[source stringByAppendingString:@"window.postMessage(a.data,a.origin||'*',a.ports.map(id=>p.get(id)));for(const id of a.ports)p.delete(id);"];source=[source stringByAppendingString:@"})()"];[s->view evaluateJavaScript:source completionHandler:^(id value,NSError*error){done(value,error.localizedDescription);}];return;}
    done(nil,[NSString stringWithFormat:@"WebKit command is unavailable: %@",op]);
}
void klj_webkit_snapshot(int64_t handle){auto s=owner(handle);if(!s||s->snapshot)return;s->snapshot=true;const int pixelWidth=s->pixelWidth,pixelHeight=s->pixelHeight;auto config=[WKSnapshotConfiguration new];config.rect=s->view.bounds;config.snapshotWidth=@(s->view.bounds.size.width);
    [s->view takeSnapshotWithConfiguration:config completionHandler:^(NSImage*image,NSError*error){s->snapshot=false;if(s->closed)return;CGImageRef cg=[image CGImageForProposedRect:nullptr context:nil hints:nil];bool ready=false;
        if(cg&&CGImageGetWidth(cg)>0&&CGImageGetHeight(cg)>0&&s->pixels.prepare(pixelWidth,pixelHeight)){CGContextDrawImage(s->pixels.context,CGRectMake(0,0,pixelWidth,pixelHeight),cg);ready=true;}
        s->client->frame(ready?s->pixels.bytes.data():nullptr,ready?pixelWidth:0,ready?pixelHeight:0,ready?nil:(error.localizedDescription?:@"WebKit did not produce an image"));
    }];
}

namespace {
// WebKit may select an editor while handling a routed event. Retain that editor
// for later Android key events without changing the native window input owner.
struct AndroidInputScope {
    Ref browser;
    explicit AndroidInputScope(Ref value):browser(value){browser->retainEditor();}
    ~AndroidInputScope(){browser->retainEditor();}
};
}
void klj_webkit_touch(int64_t handle,int action,float x,float y){
    auto s=owner(handle);if(!s)return;AndroidInputScope input(s);
    NSEventType type=action==0?NSEventTypeLeftMouseDown:action==1?NSEventTypeLeftMouseUp:NSEventTypeLeftMouseDragged;
    CGFloat pointX=x/s->density,pointY=y/s->density;
    NSPoint point=NSMakePoint(pointX,s->view.isFlipped?pointY:s->view.bounds.size.height-pointY);
    NSPoint parentPoint=[s->view convertPoint:point toView:s->view.superview];
    NSView*target=[(MLAndroidWebView*)s->view androidTargetAtPoint:parentPoint]?:s->view;
    NSEvent*event=[NSEvent mouseEventWithType:type location:[s->view convertPoint:point toView:nil] modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:s->view.window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:action==1?0:1];
    if(action==0){s->inputResponder=target;[target mouseDown:event];}else if(action==1||action==3)[target mouseUp:event];else[target mouseDragged:event];s->dirty();
}
static unsigned short macKeyCode(int key,NSString*chars){
 switch(key){case 67:return 51;case 66:return 36;case 61:return 48;case 19:return 126;case 20:return 125;case 21:return 123;case 22:return 124;case 111:return 53;case 112:return 117;}
 TISInputSourceRef input=TISCopyCurrentKeyboardLayoutInputSource();CFDataRef data=input?(CFDataRef)TISGetInputSourceProperty(input,kTISPropertyUnicodeKeyLayoutData):nullptr;unsigned short result=0xffff;
 if(data&&chars.length){auto layout=(const UCKeyboardLayout*)CFDataGetBytePtr(data);for(UInt16 code=0;code<128;code++){UInt32 dead=0;UniChar text[8];UniCharCount count=0;if(UCKeyTranslate(layout,code,kUCKeyActionDown,0,LMGetKbdType(),kUCKeyTranslateNoDeadKeysBit,&dead,8,&count,text)==noErr&&count&&[[NSString stringWithCharacters:text length:count].lowercaseString isEqualToString:chars.lowercaseString]){result=code;break;}}}
 if(input)CFRelease(input);return result;
}
void klj_webkit_key(int64_t handle,int action,int key,NSString*text,int modifiers){
    auto s=owner(handle);if(!s)return;AndroidInputScope input(s);NSString*chars=text;
    if(!chars){unichar value=key==67?NSDeleteCharacter:key==112?NSDeleteFunctionKey:key==66?'\r':key==61?'\t':key==19?NSUpArrowFunctionKey:key==20?NSDownArrowFunctionKey:key==21?NSLeftArrowFunctionKey:key==22?NSRightArrowFunctionKey:0;chars=value?[NSString stringWithCharacters:&value length:1]:@"";}
    NSEventModifierFlags flags=0;if(modifiers&1)flags|=NSEventModifierFlagShift;if(modifiers&2)flags|=NSEventModifierFlagOption;if(modifiers&4096)flags|=NSEventModifierFlagControl;if(modifiers&65536)flags|=NSEventModifierFlagCommand;
    NSEvent*event=[NSEvent keyEventWithType:action==1?NSEventTypeKeyUp:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:s->view.window.windowNumber context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:macKeyCode(key,chars)];
    NSResponder*target=s->inputResponder?:s->view;if(action==1)[target keyUp:event];else[target keyDown:event];s->dirty();
}
void klj_webkit_edit(int64_t handle,int operation,NSString*text,int start,int length){
 auto s=owner(handle);if(!s)return;AndroidInputScope input(s);id target=s->inputResponder?:s->view;NSString*value=text?:@"";
 if(operation==0&&[target respondsToSelector:@selector(insertText:replacementRange:)])[(id<NSTextInputClient>)target insertText:value replacementRange:NSMakeRange(NSNotFound,0)];
 else if(operation==1&&[target respondsToSelector:@selector(setMarkedText:selectedRange:replacementRange:)])[(id<NSTextInputClient>)target setMarkedText:value selectedRange:NSMakeRange(value.length,0) replacementRange:NSMakeRange(NSNotFound,0)];
 else if(operation==2&&[target respondsToSelector:@selector(unmarkText)])[(id<NSTextInputClient>)target unmarkText];
 else if(operation==3){NSString*script=[NSString stringWithFormat:@"(()=>{const e=document.activeElement;if(typeof e?.setSelectionRange==='function')e.setSelectionRange(%d,%d);})()",start,length];[s->view evaluateJavaScript:script inFrame:s->editorFrame inContentWorld:WKContentWorld.pageWorld completionHandler:nil];}
 else if(operation==4&&[target respondsToSelector:@selector(deleteBackward:)]){for(int i=0;i<start;i++)[target deleteBackward:nil];for(int i=0;i<length;i++)[target deleteForward:nil];}
 else if(operation==5&&[target respondsToSelector:@selector(selectAll:)])[target selectAll:nil];
 else if(operation==6&&[target respondsToSelector:@selector(setMarkedText:selectedRange:replacementRange:)])[(id<NSTextInputClient>)target setMarkedText:value selectedRange:NSMakeRange(std::min<NSUInteger>(std::max(0,start),value.length),std::min<NSUInteger>(std::max(0,length),value.length-std::min<NSUInteger>(std::max(0,start),value.length))) replacementRange:NSMakeRange(NSNotFound,0)];
 s->dirty();
}
namespace {
// Android has already applied its editing rules to the Editable. This boundary
// mirrors the resulting UTF-16 ranges through the actual document input client.
void publishEditor(const Ref&s){
 if(s->closed||s->editorPending)return;
 while(!s->editorChanges.empty()){
  EditorChange change=s->editorChanges.front();s->editorChanges.pop_front();
  if(change.frame!=s->editorFrame||![change.identity isEqual:s->editorIdentity])continue;
  s->editorPending=true;
  NSDictionary*m=change.metadata;
  NSString*begin=[NSString stringWithFormat:@"window.__maclauncherEditorBegin?.(%@,%@)??false",m[@"revision"],json(m[@"identity"])];
  [s->view evaluateJavaScript:begin inFrame:change.frame inContentWorld:WKContentWorld.pageWorld completionHandler:^(id accepted,NSError*error){
   if(s->closed)return;
   if(error||![accepted boolValue]||change.frame!=s->editorFrame){s->editorPending=false;publishEditor(s);return;}
   AndroidInputScope input(s);id target=s->inputResponder?:s->view;
   int oldStart=[m[@"beforeStart"]intValue],oldEnd=[m[@"beforeEnd"]intValue],oldFrom=[m[@"beforeComposingStart"]intValue],oldTo=[m[@"beforeComposingEnd"]intValue];
   int start=[m[@"start"]intValue],end=[m[@"end"]intValue],from=[m[@"composingStart"]intValue],to=[m[@"composingEnd"]intValue];
   bool marked=from>=0&&to>=from&&(NSUInteger)to<=change.after.length;
   NSRange replacement=oldFrom>=0&&oldTo>=oldFrom?NSMakeRange(oldFrom,oldTo-oldFrom):NSMakeRange(std::min(oldStart,oldEnd),std::abs(oldEnd-oldStart));
   bool originalRange=replacement.location<=change.before.length&&replacement.length<=change.before.length-replacement.location;
   NSString*composition=marked?[change.after substringWithRange:NSMakeRange(from,to-from)]:nil;
   bool markOnly=marked&&originalRange&&[[change.before stringByReplacingCharactersInRange:replacement withString:composition]isEqual:change.after];
   if(markOnly&&[target respondsToSelector:@selector(setMarkedText:selectedRange:replacementRange:)]){
    NSUInteger a=std::clamp(start-from,0,to-from),b=std::clamp(end-from,0,to-from);
    [(id<NSTextInputClient>)target setMarkedText:composition selectedRange:NSMakeRange(std::min(a,b),a>b?a-b:b-a) replacementRange:replacement];
   }else{
    if(oldFrom>=0&&[target respondsToSelector:@selector(unmarkText)])[(id<NSTextInputClient>)target unmarkText];
    if(![change.before isEqual:change.after]){
     NSUInteger prefix=0,suffix=0;
     while(prefix<std::min(change.before.length,change.after.length)&&[change.before characterAtIndex:prefix]==[change.after characterAtIndex:prefix])prefix++;
     auto splitPair=[](NSString*text,NSUInteger position){return position>0&&position<text.length&&CFStringIsSurrogateHighCharacter([text characterAtIndex:position-1])&&CFStringIsSurrogateLowCharacter([text characterAtIndex:position]);};
     if(splitPair(change.before,prefix)||splitPair(change.after,prefix))prefix--;
     while(suffix<change.before.length-prefix&&suffix<change.after.length-prefix&&[change.before characterAtIndex:change.before.length-suffix-1]==[change.after characterAtIndex:change.after.length-suffix-1])suffix++;
     if(splitPair(change.before,change.before.length-suffix)||splitPair(change.after,change.after.length-suffix))suffix--;
     NSString*text=[change.after substringWithRange:NSMakeRange(prefix,change.after.length-prefix-suffix)];
     if([target respondsToSelector:@selector(insertText:replacementRange:)])[(id<NSTextInputClient>)target insertText:text replacementRange:NSMakeRange(prefix,change.before.length-prefix-suffix)];
    }
    if(marked&&[target respondsToSelector:@selector(setMarkedText:selectedRange:replacementRange:)]){
     NSUInteger a=std::clamp(start-from,0,to-from),b=std::clamp(end-from,0,to-from);
     [(id<NSTextInputClient>)target setMarkedText:composition selectedRange:NSMakeRange(std::min(a,b),a>b?a-b:b-a) replacementRange:NSMakeRange(from,to-from)];
    }
   }
   NSString*finish=[NSString stringWithFormat:@"window.__maclauncherEditorEnd?.(%d,%d,%d,%d)",start,end,from,to];
   [s->view evaluateJavaScript:finish inFrame:change.frame inContentWorld:WKContentWorld.pageWorld completionHandler:^(id value,NSError*failure){if(s->closed)return;s->editorPending=false;s->dirty();publishEditor(s);}];
  }];
  return;
 }
}
}
void klj_webkit_editor(int64_t handle,NSString*before,NSString*after,NSString*metadata){
 auto s=owner(handle);if(!s)return;
 NSDictionary*m=[NSJSONSerialization JSONObjectWithData:[metadata dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
 if(!before||!after||![m isKindOfClass:NSDictionary.class]||![m[@"identity"]isKindOfClass:NSString.class]||![m[@"revision"]isKindOfClass:NSNumber.class]){fail("WebKit editor state is invalid");return;}
 if(!s->editorFrame)return;
 s->editorChanges.push_back({before,after,m,s->editorFrame,s->editorIdentity});publishEditor(s);
}
void klj_webkit_setting(int64_t handle,NSString*name,NSString*encoded){auto s=owner(handle);if(!s)return;NSString*key=name;id value=[NSJSONSerialization JSONObjectWithData:[encoded dataUsingEncoding:NSUTF8StringEncoding] options:NSJSONReadingFragmentsAllowed error:nil];
    if([key isEqualToString:@"JavaScriptEnabled"])s->view.configuration.preferences.javaScriptEnabled=[value boolValue];
    else if([key isEqualToString:@"JavaScriptCanOpenWindowsAutomatically"])s->view.configuration.preferences.javaScriptCanOpenWindowsAutomatically=[value boolValue];
    else if([key isEqualToString:@"UserAgentString"])s->view.customUserAgent=value==[NSNull null]?nil:value;
    else if([key isEqualToString:@"MinimumFontSize"])s->view.configuration.preferences.minimumFontSize=[value doubleValue];
    else if([key isEqualToString:@"TextZoom"])[s->view _setTextZoomFactor:std::max(.01,[value doubleValue]/100.)];
    else if([key isEqualToString:@"PageZoom"])s->view.pageZoom=std::max(.01,[value doubleValue]);
    else if([key isEqualToString:@"BackgroundColor"]){uint32_t c=[value unsignedIntValue];s->view.underPageBackgroundColor=[NSColor colorWithSRGBRed:((c>>16)&255)/255. green:((c>>8)&255)/255. blue:(c&255)/255. alpha:(c>>24)/255.];}
    else if([key isEqualToString:@"WebContentsDebuggingEnabled"]){if(@available(macOS 13.3,*))s->view.inspectable=[value boolValue];}
    s->dirty();
}

NSString* klj_webkit_cookies(std::shared_ptr<KljWebContext> context,NSString*address){NSArray<NSHTTPCookie*>*snapshot=currentCookies(context);if(!address)return snapshot.count?@"cookies":nil;NSURL*url=[NSURL URLWithString:address];if(!url)return nullptr;NSMutableArray<NSHTTPCookie*>*matched=[NSMutableArray new];NSString*host=url.host.lowercaseString,*path=url.path.length?url.path:@"/";NSDate*now=NSDate.date;for(NSHTTPCookie*cookie in snapshot){NSString*domain=cookie.domain.lowercaseString;BOOL domainMatches=[host isEqualToString:domain]||([domain hasPrefix:@"."]&&([host isEqualToString:[domain substringFromIndex:1]]||[host hasSuffix:domain]));NSString*cookiePath=cookie.path.length?cookie.path:@"/";BOOL pathMatches=[path isEqualToString:cookiePath]||([path hasPrefix:cookiePath]&&([cookiePath hasSuffix:@"/"]||(path.length>cookiePath.length&&[path characterAtIndex:cookiePath.length]=='/')));if(domainMatches&&pathMatches&&(!cookie.secure||[url.scheme.lowercaseString isEqualToString:@"https"])&&(!cookie.expiresDate||[cookie.expiresDate compare:now]==NSOrderedDescending))[matched addObject:cookie];}return [NSHTTPCookie requestHeaderFieldsWithCookies:matched][@"Cookie"];}
void klj_webkit_cookieCommand(std::shared_ptr<KljWebContext> context,NSString*operation,NSString*encoded,std::shared_ptr<KljWebResult> callback){NSString*op=operation;id body=[NSJSONSerialization JSONObjectWithData:[encoded dataUsingEncoding:NSUTF8StringEncoding] options:NSJSONReadingFragmentsAllowed error:nil];auto completion=std::make_shared<Completion>(std::move(callback));dispatch_async(dispatch_get_main_queue(),^{try{ensureStore(context);}catch(const std::exception&error){completion->done(nil,@(error.what()));return;}auto cookies=context->store.httpCookieStore;
    if([op isEqualToString:@"set"]){NSURL*url=[NSURL URLWithString:body[@"url"]];NSArray<NSHTTPCookie*>*values=url?[NSHTTPCookie cookiesWithResponseHeaderFields:@{@"Set-Cookie":body[@"value"]?:@""} forURL:url]:@[];if(!values.count){completion->done(@NO);return;}dispatch_group_t group=dispatch_group_create();for(NSHTTPCookie*cookie in values){dispatch_group_enter(group);[cookies setCookie:cookie completionHandler:^{dispatch_group_leave(group);}];}dispatch_group_notify(group,dispatch_get_main_queue(),^{refreshCookies(context,^{completion->done(@YES);});});
    }else if([op isEqualToString:@"removeAll"]||[op isEqualToString:@"removeSession"]||[op isEqualToString:@"removeExpired"]){[cookies getAllCookies:^(NSArray<NSHTTPCookie*>*values){dispatch_group_t group=dispatch_group_create();__block BOOL removed=NO;for(NSHTTPCookie*cookie in values)if([op isEqualToString:@"removeAll"]||([op isEqualToString:@"removeSession"]&&cookie.sessionOnly)||([op isEqualToString:@"removeExpired"]&&cookie.expiresDate&&[cookie.expiresDate compare:NSDate.date]!=NSOrderedDescending)){removed=YES;dispatch_group_enter(group);[cookies deleteCookie:cookie completionHandler:^{dispatch_group_leave(group);}];}dispatch_group_notify(group,dispatch_get_main_queue(),^{refreshCookies(context,^{completion->done(@(removed));});});}];
    }else if([op isEqualToString:@"hasCookies"]){[cookies getAllCookies:^(NSArray<NSHTTPCookie*>*values){completion->done(@(values.count>0));}];
    }else if([op isEqualToString:@"accept"]){if(@available(macOS 14.0,*))[cookies setCookiePolicy:[body[@"allowed"]boolValue]?WKCookiePolicyAllow:WKCookiePolicyDisallow completionHandler:^{completion->done(@YES);}];else completion->done(nil,@"Cookie policy control requires macOS 14");
    }else if([op isEqualToString:@"flush"]){refreshCookies(context,^{completion->done(@YES);});
    }else completion->done(nil,[NSString stringWithFormat:@"Unknown WebKit cookie command: %@",op]);});}
void klj_webkit_storageCommand(std::shared_ptr<KljWebContext> context,NSString*operation,NSString*encoded,std::shared_ptr<KljWebResult> callback){NSString*op=operation;id body=[NSJSONSerialization JSONObjectWithData:[encoded dataUsingEncoding:NSUTF8StringEncoding]options:NSJSONReadingFragmentsAllowed error:nil];auto completion=std::make_shared<Completion>(std::move(callback));dispatch_async(dispatch_get_main_queue(),^{try{ensureStore(context);}catch(const std::exception&error){completion->done(nil,@(error.what()));return;}NSSet*types=WKWebsiteDataStore.allWebsiteDataTypes;
    if([op isEqualToString:@"deleteAll"]){[context->store removeDataOfTypes:types modifiedSince:[NSDate distantPast] completionHandler:^{completion->done(@YES);}];return;}
    [context->store fetchDataRecordsOfTypes:types completionHandler:^(NSArray<WKWebsiteDataRecord*>*records){if([op isEqualToString:@"origins"]){NSMutableArray*values=[NSMutableArray new];for(WKWebsiteDataRecord*record in records)[values addObject:@{@"origin":record.displayName,@"types":record.dataTypes.allObjects,@"usage":@0,@"quota":@0}];completion->done(values);
        }else if([op isEqualToString:@"deleteOrigin"]){NSString*origin=body[@"origin"],*host=[NSURL URLWithString:origin].host?:origin;NSMutableArray*selected=[NSMutableArray new];for(WKWebsiteDataRecord*record in records)if([record.displayName isEqualToString:host])[selected addObject:record];[context->store removeDataOfTypes:types forDataRecords:selected completionHandler:^{completion->done(@YES);}];
        }else completion->done(nil,@"WebKit does not expose persistent storage byte quotas through its public API");}];});}
