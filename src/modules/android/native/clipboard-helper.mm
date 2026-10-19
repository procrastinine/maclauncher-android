#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <libproc.h>
#include <fcntl.h>
#include <unistd.h>
#include <set>
#include <map>
#include <vector>
#include <thread>
#include "clipboard-gesture.h"

namespace {
constexpr size_t limit=1048576;
KLClipboardGestures gestures;
std::vector<CGKeyCode> copyKeys,pasteKeys;
std::map<pid_t,std::pair<uint64_t,uint64_t>> owners;
pid_t foreground=0;
dispatch_source_t timer;

pid_t ownerOf(pid_t pid) {
 std::set<pid_t> visited;
 while(pid>1&&visited.insert(pid).second){
  proc_bsdinfo info{};
  if(proc_pidinfo(pid,PROC_PIDTBSDINFO,0,&info,sizeof info)!=sizeof info||info.pbi_uid!=getuid())return 0;
  auto owner=owners.find(pid);
  if(owner!=owners.end())return owner->second==std::make_pair(info.pbi_start_tvsec,info.pbi_start_tvusec)?pid:0;
  pid=info.pbi_ppid;
 }
 return 0;
}
void keyboardLayout(){
 copyKeys.clear();pasteKeys.clear();
 TISInputSourceRef source=TISCopyCurrentKeyboardLayoutInputSource();
 CFDataRef data=source?(CFDataRef)TISGetInputSourceProperty(source,kTISPropertyUnicodeKeyLayoutData):nullptr;
 if(data){auto layout=(const UCKeyboardLayout*)CFDataGetBytePtr(data);
  for(UInt16 code=0;code<128;code++){
   UniChar text[8];UniCharCount length=0;UInt32 dead=0;
   if(UCKeyTranslate(layout,code,kUCKeyActionDown,cmdKey>>8,LMGetKbdType(),kUCKeyTranslateNoDeadKeysBit,&dead,8,&length,text)==noErr&&length==1){
    UniChar key=text[0]|32;if(key=='c'||key=='x')copyKeys.push_back(code);else if(key=='v')pasteKeys.push_back(code);
   }
  }
 }
 if(source)CFRelease(source);
}
bool down(const std::vector<CGKeyCode>&keys){for(auto code:keys)if(CGEventSourceKeyState(kCGEventSourceStateHIDSystemState,code))return true;return false;}
void sample(){
 pid_t owner=ownerOf(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier);
 CGEventFlags flags=CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
 gestures.sample(owner,flags&kCGEventFlagMaskCommand,flags&(kCGEventFlagMaskControl|kCGEventFlagMaskAlternate),down(copyKeys),down(pasteKeys),NSProcessInfo.processInfo.systemUptime);
 foreground=owner;
}
void updateObserver(){
 sample();
 dispatch_source_set_timer(timer,foreground?DISPATCH_TIME_NOW:DISPATCH_TIME_FOREVER,foreground?4*NSEC_PER_MSEC:DISPATCH_TIME_FOREVER,NSEC_PER_MSEC);
}
NSDictionary* transfer(pid_t peer,NSDictionary*message){
 NSString*operation=message[@"operation"];
 if(![operation isEqual:@"copy"]&&![operation isEqual:@"paste"])return @{@"allowed":@NO};
 pid_t owner=ownerOf(peer);updateObserver();
 if(!owner||!gestures.consume(owner,foreground,[operation isEqual:@"copy"],NSProcessInfo.processInfo.systemUptime))return @{@"allowed":@NO};
 if([operation isEqual:@"paste"]){
  NSString*text=[NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
  if(!text||[text lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>limit-512)return @{@"allowed":@NO};
  return @{@"allowed":@YES,@"text":text};
 }
 NSString*text=message[@"text"];
 if(![text isKindOfClass:NSString.class]||[text lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>limit-512)return @{@"allowed":@NO};
 [NSPasteboard.generalPasteboard clearContents];
 return @{@"allowed":@([NSPasteboard.generalPasteboard setString:text forType:NSPasteboardTypeString])};
}
void client(int fd){@autoreleasepool{
 timeval timeout{2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
 int noSig=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&noSig,sizeof noSig);
 pid_t peer=0;socklen_t size=sizeof peer;
 if(getsockopt(fd,SOL_LOCAL,LOCAL_PEERPID,&peer,&size)!=0||peer<=0){close(fd);return;}
 NSMutableData*pending=[NSMutableData new];unsigned char bytes[8192];bool complete=false;
 while(pending.length<=limit){ssize_t count=read(fd,bytes,sizeof bytes);if(count<=0)break;[pending appendBytes:bytes length:size_t(count)];
  const void*newline=memchr(pending.bytes,'\n',pending.length);if(newline){size_t length=(const char*)newline-(const char*)pending.bytes;if(length>limit)break;[pending setLength:length];complete=true;break;}}
 __block NSData*result=nil;
 if(complete){id message=[NSJSONSerialization JSONObjectWithData:pending options:0 error:nil];
  dispatch_sync(dispatch_get_main_queue(),^{@autoreleasepool{@try{
   NSDictionary*reply=[message isKindOfClass:NSDictionary.class]?transfer(peer,message):@{@"allowed":@NO};
   result=[NSJSONSerialization dataWithJSONObject:reply options:0 error:nil];
  }@catch(NSException*){}}});}
 if(!result||result.length>limit)result=[@"{\"allowed\":false}" dataUsingEncoding:NSUTF8StringEncoding];
 NSMutableData*line=[result mutableCopy];[line appendBytes:"\n" length:1];
 size_t offset=0;while(offset<line.length){ssize_t sent=write(fd,(const char*)line.bytes+offset,line.length-offset);if(sent<=0)break;offset+=sent;}
 close(fd);
}}
void control(NSData*bytes){
 id message=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];if(![message isKindOfClass:NSDictionary.class])return;
 pid_t pid=[message[@"pid"] respondsToSelector:@selector(intValue)]?[message[@"pid"]intValue]:0;
 if([message[@"op"]isEqual:@"enroll"]){proc_bsdinfo info{};
  if(pid>1&&proc_pidinfo(pid,PROC_PIDTBSDINFO,0,&info,sizeof info)==sizeof info&&info.pbi_uid==getuid()){
   owners[pid]={info.pbi_start_tvsec,info.pbi_start_tvusec};gestures.enroll(pid);updateObserver();}}
 else if([message[@"op"]isEqual:@"retire"]){owners.erase(pid);gestures.retire(pid);updateObserver();}
}
}
int main(int argc,char**argv){@autoreleasepool{
 if(argc==2&&strcmp(argv[1],"--peer-pid")==0){
  pid_t peer=0;socklen_t size=sizeof peer;uid_t uid=0;gid_t gid=0;
  if(getsockopt(STDIN_FILENO,SOL_LOCAL,LOCAL_PEERPID,&peer,&size)!=0||peer<=0||getpeereid(STDIN_FILENO,&uid,&gid)!=0||uid!=getuid())return 2;
  printf("{\"pid\":%d}\n",peer);return 0;
 }
 if(argc!=2||strlen(argv[1])>=sizeof(sockaddr_un{}.sun_path))return 2;
 int server=socket(AF_UNIX,SOCK_STREAM,0);if(server<0)return 3;
 sockaddr_un address{};address.sun_family=AF_UNIX;strlcpy(address.sun_path,argv[1],sizeof address.sun_path);
 if(bind(server,(sockaddr*)&address,sizeof address)!=0||chmod(argv[1],0600)!=0||listen(server,SOMAXCONN)!=0)return 4;
 keyboardLayout();timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
 dispatch_source_set_timer(timer,DISPATCH_TIME_FOREVER,DISPATCH_TIME_FOREVER,NSEC_PER_MSEC);
 dispatch_source_set_event_handler(timer,^{@autoreleasepool{sample();}});dispatch_resume(timer);
 [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceDidActivateApplicationNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*){updateObserver();}];
 [NSDistributedNotificationCenter.defaultCenter addObserverForName:(__bridge NSString*)kTISNotifySelectedKeyboardInputSourceChanged object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*){keyboardLayout();}];
 std::thread([server]{for(;;){int fd=accept(server,nullptr,nullptr);if(fd>=0)std::thread(client,fd).detach();else if(errno!=EINTR)break;}}).detach();
 fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO,F_GETFL)|O_NONBLOCK);
 NSMutableData*pending=[NSMutableData new];
 dispatch_source_t input=dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,STDIN_FILENO,0,dispatch_get_main_queue());
 dispatch_source_set_event_handler(input,^{@autoreleasepool{
  unsigned char buffer[8192];ssize_t count=read(STDIN_FILENO,buffer,sizeof buffer);
  if(count==0){unlink(argv[1]);_exit(0);}if(count<0){if(errno!=EAGAIN&&errno!=EINTR){unlink(argv[1]);_exit(1);}return;}
  [pending appendBytes:buffer length:size_t(count)];
  for(;;){const void*newline=memchr(pending.bytes,'\n',pending.length);if(!newline)break;size_t length=(const char*)newline-(const char*)pending.bytes;if(length>4096)_exit(1);
   control([pending subdataWithRange:NSMakeRange(0,length)]);[pending replaceBytesInRange:NSMakeRange(0,length+1) withBytes:nullptr length:0];}
  if(pending.length>4096)_exit(1);
 }});dispatch_resume(input);
 write(STDOUT_FILENO,"ready\n",6);CFRunLoopRun();
}}
