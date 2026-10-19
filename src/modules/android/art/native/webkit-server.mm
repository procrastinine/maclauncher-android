#import <Foundation/Foundation.h>
#include "webkit-physical-binding.h"
#include "webkit-socket.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <map>
#include <set>

namespace klj::web {
namespace {
struct Scope {NSString*root=nil;NSString*guest=nil;bool network=false;};
struct Connection;
int listener=-1,registrations=-1;void*contentView=nullptr;
std::string socketPath;std::vector<uint8_t>registrationBytes;
std::map<std::u16string,Scope> scopes;
std::map<std::u16string,std::weak_ptr<KljWebContext>> contexts;
std::map<int,std::shared_ptr<Connection>> connections;
Value text(NSString*s){if(!s)return {};std::u16string v(s.length,0);[s getCharacters:reinterpret_cast<unichar*>(v.data()) range:NSMakeRange(0,s.length)];return Value(std::move(v));}
NSString* text(const Value&v){return v.type==Value::text?[[NSString alloc]initWithCharacters:reinterpret_cast<const unichar*>(v.string.data()) length:v.string.size()]:nil;}
NSString* translate(NSString*value,NSString*from,NSString*to){if([value isEqualToString:from])return to;NSString*prefix=[from stringByAppendingString:@"/"];return [value hasPrefix:prefix]?[to stringByAppendingString:[value substringFromIndex:from.length]]:value;}
NSString* url(NSString*value,NSString*from,NSString*to){NSURL*address=[NSURL URLWithString:value];return address.isFileURL?[NSURL fileURLWithPath:translate(address.path,from,to)].absoluteString:value;}
struct Remote final:Callback {
 std::weak_ptr<Connection>connection;uint64_t callback;
 Remote(std::weak_ptr<Connection>c,uint64_t id):connection(std::move(c)),callback(id){}
 Value invoke(Event,const Args&)override;
};
struct Connection:std::enable_shared_from_this<Connection> {
 int fd;bool authenticated=false,closed=false;Scope scope;std::u16string token;Frames frames;
 std::shared_ptr<KljWebContext>context;std::set<int64_t> handles;uint64_t next=1;
 std::map<uint64_t,Frame> replies;
 explicit Connection(int f):fd(f){}
 void close(){if(closed)return;closed=true;::shutdown(fd,SHUT_RDWR);::close(fd);auto owned=std::move(handles);for(auto handle:owned)klj_webkit_destroy(handle);context.reset();}
 ~Connection(){close();}
 void handle(Frame frame){
  auto kind=Message(frame.header.kind);
  if(!authenticated){if(kind!=Message::hello||frame.value.type!=Value::text)throw std::runtime_error("Browser connection has no application capability");auto found=scopes.find(frame.value.string);if(found==scopes.end())throw std::runtime_error("Browser application capability is unavailable");scope=found->second;token=frame.value.string;auto key=text(scope.root).string;auto&shared=contexts[key];context=shared.lock();if(!context){context=klj_webkit_context();shared=context;}authenticated=true;return;}
  if(kind==Message::eventReply||kind==Message::error){replies.emplace(frame.header.id,std::move(frame));return;}
  if(kind!=Message::request||frame.value.type!=Value::list||frame.header.operation>uint32_t(Op::storageCommand))throw std::runtime_error("Invalid browser operation packet");
  try{
   Op op=Op(frame.header.operation);Args a=std::move(frame.value.items);
   if(op!=Op::initialize&&op!=Op::create&&op!=Op::cookies&&op!=Op::cookieCommand&&op!=Op::storageCommand&&!handles.contains(a.at(0).number))throw std::runtime_error("WebKit view does not belong to this process");
   if(op==Op::initialize){
    NSString*directory=text(a.at(0)),*root=text(a.at(1));
    if(![root isEqualToString:scope.guest]||![directory isEqualToString:[scope.guest stringByAppendingPathComponent:@"files/webview"]])throw std::runtime_error("Browser storage differs from its application capability");
    a[0]=text([scope.root stringByAppendingPathComponent:@"files/webview"]);a[1]=text(scope.root);a[2]=Value(scope.network);
   }else if(op==Op::load||op==Op::html){const size_t index=op==Op::load?1:2;if(a.at(index).type==Value::text)a[index]=text(url(text(a[index]),scope.guest,scope.root));}
   else if(op==Op::command&&[text(a.at(1))isEqualToString:@"archive"]){NSData*bytes=[text(a.at(2))dataUsingEncoding:NSUTF8StringEncoding];id raw=bytes?[NSJSONSerialization JSONObjectWithData:bytes options:NSJSONReadingMutableContainers error:nil]:nil;if([raw isKindOfClass:NSMutableDictionary.class]&&[raw[@"path"]isKindOfClass:NSString.class]){raw[@"path"]=translate(raw[@"path"],scope.guest,scope.root);NSData*encoded=[NSJSONSerialization dataWithJSONObject:raw options:0 error:nil];a[2]=text([[NSString alloc]initWithData:encoded encoding:NSUTF8StringEncoding]);}}
   auto callback=frame.header.callback?std::make_shared<Remote>(weak_from_this(),frame.header.callback):nullptr;
   auto result=physicalRequest(context,op,a,std::move(callback),contentView);
   if(op==Op::create)handles.insert(result.number);else if(op==Op::destroy)handles.erase(a.at(0).number);
   sendFrame(fd,Message::reply,0,frame.header.id,0,result);
  }catch(const std::exception&error){sendFrame(fd,Message::error,0,frame.header.id,0,errorValue(error.what()));}
 }
 void poll(){Frame frame;while(frames.next(frame))handle(std::move(frame));while(frames.read(fd))while(frames.next(frame))handle(std::move(frame));}
 Value event(Event event,const Args&arguments,uint64_t callback){
  if(closed)return {};uint64_t id=requiresReply(event)?next++:0;Args a=arguments;
  if(event==Event::navigation&&a.size()>1)a[1]=text(url(text(a[1]),scope.root,scope.guest));
  else if(event==Event::navigationRequest&&!a.empty())a[0]=text(url(text(a[0]),scope.root,scope.guest));
  else if(event==Event::history)for(auto&item:a.at(0).items)item=text(url(text(item),scope.root,scope.guest));
  else if(event==Event::dialog&&a.size()>2)a[2]=text(url(text(a[2]),scope.root,scope.guest));
  sendFrame(fd,Message::event,uint32_t(event),id,callback,Value(std::move(a)));
  if(!id)return {};
  // Java may reenter WebView from a navigation/dialog callback. Consume those
  // requests on the same AppKit owner while waiting for the original reply.
  while(!closed){auto found=replies.find(id);if(found!=replies.end()){Frame reply=std::move(found->second);replies.erase(found);if(Message(reply.header.kind)==Message::error)throw std::runtime_error(errorString(reply.value));return reply.value;}handle(frames.wait(fd));}
  return {};
 }
};
Value Remote::invoke(Event event,const Args&a){auto owner=connection.lock();if(!owner||owner->closed)return {};try{return owner->event(event,a,callback);}catch(const std::exception&error){fprintf(stderr,"[android-webkit] Callback transport: %s\n",error.what());owner->close();return {};}}
void registerScopes(){
 uint8_t buffer[8192];for(;;){ssize_t n=read(registrations,buffer,sizeof(buffer));if(n>0)registrationBytes.insert(registrationBytes.end(),buffer,buffer+n);else if(n<0&&errno==EINTR)continue;else if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;else throw std::runtime_error("Browser application owner channel closed");}
 size_t consumed=0;while(registrationBytes.size()-consumed>=4){uint32_t length;memcpy(&length,registrationBytes.data()+consumed,4);if(length>registrationBytes.size()-consumed-4)break;NSData*data=[NSData dataWithBytes:registrationBytes.data()+consumed+4 length:length];NSDictionary*value=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];NSString*key=value[@"token"];if(![key isKindOfClass:NSString.class])throw std::runtime_error("Invalid browser application registration");auto token=text(key).string;
  if([value[@"retire"]boolValue]){scopes.erase(token);for(auto&row:connections)if(row.second->token==token)row.second->close();}
  else{NSString*root=value[@"root"],*guest=value[@"guest"];if(![root isKindOfClass:NSString.class]||![guest isKindOfClass:NSString.class]||![root isAbsolutePath]||![guest hasPrefix:@"/data/user/0/"])throw std::runtime_error("Invalid browser application storage registration");for(auto it=contexts.begin();it!=contexts.end();)if(it->second.expired())it=contexts.erase(it);else ++it;scopes.emplace(std::move(token),Scope{root,guest,[value[@"network"]boolValue]});}
  consumed+=4+length;
 }if(consumed)registrationBytes.erase(registrationBytes.begin(),registrationBytes.begin()+consumed);
}
}
}
extern "C" int klj_art_webkit_open(const char*path,int boundSocket,int registrationFd,void*parent){using namespace klj::web;try{
 if(listener>=0||!path||!parent)throw std::runtime_error("Browser window owner is already configured or absent");
 // AIM's original unixsock::bind_at supplies this descriptor, including long
 // mapped paths, without changing another thread's working directory.
 listener=boundSocket;if(listener<0||chmod(path,0600)||listen(listener,16))throw std::runtime_error("Browser window socket could not be opened");
 for(int fd:{listener,registrationFd}){int flags=fcntl(fd,F_GETFL);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0||fcntl(fd,F_SETFD,FD_CLOEXEC)<0)throw std::runtime_error("Browser window capability could not be configured");}
 registrations=registrationFd;contentView=parent;socketPath=path;return 0;
 }catch(const std::exception&error){fprintf(stderr,"[android-webkit] %s\n",error.what());if(listener>=0)close(listener);listener=-1;return -1;}}
extern "C" int klj_art_webkit_poll(){using namespace klj::web;if(listener<0)return 0;try{
 registerScopes();for(;;){int fd=accept(listener,nullptr,nullptr);if(fd<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK)break;throw std::runtime_error("Browser window accept failed");}uid_t uid;gid_t gid;int yes=1,flags=fcntl(fd,F_GETFL);if(getpeereid(fd,&uid,&gid)||uid!=geteuid()||flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0||setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes))<0){close(fd);continue;}fcntl(fd,F_SETFD,FD_CLOEXEC);connections.emplace(fd,std::make_shared<Connection>(fd));}
 for(auto it=connections.begin();it!=connections.end();){auto owner=it->second;try{if(!owner->closed)owner->poll();}catch(const std::exception&error){fprintf(stderr,"[android-webkit] Connection: %s\n",error.what());owner->close();}if(owner->closed)it=connections.erase(it);else ++it;}return 0;
 }catch(const std::exception&error){fprintf(stderr,"[android-webkit] Window transport: %s\n",error.what());return -1;}}
extern "C" void klj_art_webkit_close(){using namespace klj::web;for(auto&row:connections)row.second->close();connections.clear();scopes.clear();contexts.clear();registrationBytes.clear();if(listener>=0)close(listener);listener=-1;if(!socketPath.empty())unlink(socketPath.c_str());socketPath.clear();registrations=-1;contentView=nullptr;}
