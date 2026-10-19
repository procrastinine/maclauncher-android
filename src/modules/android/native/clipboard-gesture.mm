#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include "clipboard-gesture.h"

// One launcher-owned observer lives only for its enrolled Android processes.
// It observes only hardware Command/C/V, never typed text or pasteboard data.
// Explicit requests consume an expiring gesture recorded before IPC latency.
namespace {
constexpr size_t limit=1048576;
KLClipboardGestures gestures;
std::vector<CGKeyCode> copyKeys,pasteKeys;
pid_t foreground=0;
dispatch_source_t timer;
void keyboardLayout(){
 copyKeys.clear();pasteKeys.clear();
 TISInputSourceRef source=TISCopyCurrentKeyboardLayoutInputSource();
 CFDataRef data=source?(CFDataRef)TISGetInputSourceProperty(source,kTISPropertyUnicodeKeyLayoutData):nullptr;
 if(data){auto layout=(const UCKeyboardLayout*)CFDataGetBytePtr(data);
  for(UInt16 code=0;code<128;code++){
   UniChar text[8];UniCharCount length=0;UInt32 dead=0;
   if(UCKeyTranslate(layout,code,kUCKeyActionDown,cmdKey>>8,LMGetKbdType(),kUCKeyTranslateNoDeadKeysBit,&dead,8,&length,text)==noErr&&length==1){
    UniChar key=text[0]|32;if(key=='c')copyKeys.push_back(code);else if(key=='v')pasteKeys.push_back(code);
   }
  }
 }
 if(source)CFRelease(source);
}
bool down(const std::vector<CGKeyCode>&keys){for(auto code:keys)if(CGEventSourceKeyState(kCGEventSourceStateHIDSystemState,code))return true;return false;}
void sample(){
 if(!gestures.contains(foreground)){gestures.sample(0,false,false,false,false,NSProcessInfo.processInfo.systemUptime);return;}
 CGEventFlags flags=CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
 gestures.sample(foreground,flags&kCGEventFlagMaskCommand,flags&(kCGEventFlagMaskControl|kCGEventFlagMaskAlternate),down(copyKeys),down(pasteKeys),NSProcessInfo.processInfo.systemUptime);
}
void updateObserver(){
 foreground=NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
 const bool active=gestures.contains(foreground);sample();
 dispatch_source_set_timer(timer,active?DISPATCH_TIME_NOW:DISPATCH_TIME_FOREVER,active?4*NSEC_PER_MSEC:DISPATCH_TIME_FOREVER,NSEC_PER_MSEC);
}
void reply(NSDictionary*value){
 NSData*bytes=[NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
 if(bytes.length>limit-256)bytes=[NSJSONSerialization dataWithJSONObject:@{@"id":value[@"id"]?:@"",@"allowed":@NO} options:0 error:nil];
 [NSFileHandle.fileHandleWithStandardOutput writeData:bytes];[NSFileHandle.fileHandleWithStandardOutput writeData:[NSData dataWithBytes:"\n" length:1]];
}
void request(NSData*bytes){
 id message=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];
 if(![message isKindOfClass:NSDictionary.class])return;
 NSString*op=message[@"op"];pid_t pid=[message[@"pid"] respondsToSelector:@selector(intValue)]?[message[@"pid"]intValue]:0;
 if([op isEqual:@"enroll"]){gestures.enroll(pid);updateObserver();return;}
 if([op isEqual:@"retire"]){gestures.retire(pid);updateObserver();return;}
 NSString*identifier=message[@"id"];if(![identifier isKindOfClass:NSString.class]||identifier.length>32)return;
 NSMutableDictionary*result=[@{@"id":identifier,@"allowed":@NO} mutableCopy];
 NSString*operation=message[@"operation"];
 if([op isEqual:@"transfer"]&&([operation isEqual:@"copy"]||[operation isEqual:@"paste"])){
  updateObserver(); // Resynchronize activation and cover the next hardware tick.
  if(gestures.consume(pid,foreground,[operation isEqual:@"copy"],NSProcessInfo.processInfo.systemUptime)){
   if([operation isEqual:@"paste"]){NSString*text=[NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
    if(text&&[text lengthOfBytesUsingEncoding:NSUTF8StringEncoding]<=limit-512){result[@"allowed"]=@YES;result[@"text"]=text;}
   }else if([message[@"text"]isKindOfClass:NSString.class]){
    [NSPasteboard.generalPasteboard clearContents];result[@"allowed"]=@([NSPasteboard.generalPasteboard setString:message[@"text"] forType:NSPasteboardTypeString]);result[@"text"]=@"";
   }
  }
 }
 reply(result);
}
}
int main(){@autoreleasepool{
 keyboardLayout();foreground=NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
 timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
 [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceDidActivateApplicationNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*){updateObserver();}];
 [NSDistributedNotificationCenter.defaultCenter addObserverForName:(__bridge NSString*)kTISNotifySelectedKeyboardInputSourceChanged object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*){keyboardLayout();}];
 dispatch_source_set_timer(timer,DISPATCH_TIME_FOREVER,DISPATCH_TIME_FOREVER,NSEC_PER_MSEC);
 dispatch_source_set_event_handler(timer,^{@autoreleasepool{sample();}});dispatch_resume(timer);
 fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO,F_GETFL)|O_NONBLOCK);
 NSMutableData*pending=[NSMutableData new];
 dispatch_source_t input=dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,STDIN_FILENO,0,dispatch_get_main_queue());
 dispatch_source_set_event_handler(input,^{@autoreleasepool{
  unsigned char buffer[8192];ssize_t count=read(STDIN_FILENO,buffer,sizeof buffer);
  if(count==0)_exit(0);if(count<0){if(errno!=EAGAIN&&errno!=EINTR)_exit(1);return;}
  [pending appendBytes:buffer length:size_t(count)];
  for(;;){const void*newline=memchr(pending.bytes,'\n',pending.length);if(!newline)break;size_t length=(const char*)newline-(const char*)pending.bytes;if(length>limit)_exit(1);
   @try{request([pending subdataWithRange:NSMakeRange(0,length)]);}@catch(NSException*){}
   [pending replaceBytesInRange:NSMakeRange(0,length+1) withBytes:nullptr length:0];
  }
  if(pending.length>limit)_exit(1);
 }});dispatch_resume(input);
 // Workspace activation and keyboard-layout notifications use the main run
 // loop. A dispatch-only loop can leave foreground ownership stale, preventing
 // the gesture observer from following the Android window after activation.
 CFRunLoopRun();
}}
