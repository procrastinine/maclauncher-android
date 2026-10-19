#pragma once
#include "webkit-binding.h"
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace klj::web {
enum class Message:uint32_t {hello,request,reply,event,eventReply,error};
struct Header {uint32_t magic=0x574b4d4c,kind=0,operation=0,reserved=0;uint64_t id=0,callback=0,length=0;};
struct Frame {Header header;Value value;};
inline void writeBytes(int fd,const void*data,size_t count){
 const auto*p=static_cast<const uint8_t*>(data);
 while(count){
#ifdef __APPLE__
  ssize_t n=send(fd,p,count,0);
#else
  ssize_t n=send(fd,p,count,MSG_NOSIGNAL);
#endif
  if(n>0){p+=n;count-=n;continue;}if(n<0&&errno==EINTR)continue;
  if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){pollfd item{fd,POLLOUT,0};if(poll(&item,1,-1)>0)continue;if(errno==EINTR)continue;}
  throw std::runtime_error("Browser transport closed while sending");
 }
}
inline void sendFrame(int fd,Message type,uint32_t operation,uint64_t id,uint64_t callback,const Value&value){auto bytes=encode(value);Header h;h.kind=uint32_t(type);h.operation=operation;h.id=id;h.callback=callback;h.length=bytes.size();writeBytes(fd,&h,sizeof(h));writeBytes(fd,bytes.data(),bytes.size());}
struct Frames {
 std::vector<uint8_t> bytes;size_t consumed=0;
 bool next(Frame&out){
  if(bytes.size()-consumed<sizeof(Header))return false;Header h;memcpy(&h,bytes.data()+consumed,sizeof(h));
  if(h.magic!=Header().magic||h.reserved||h.kind>uint32_t(Message::error)||h.length>SIZE_MAX-sizeof(Header))throw std::runtime_error("Invalid browser transport header");
  if(h.length>bytes.size()-consumed-sizeof(Header))return false;
  out={h,decode(bytes.data()+consumed+sizeof(Header),h.length)};consumed+=sizeof(Header)+h.length;
  if(consumed==bytes.size()){bytes.clear();consumed=0;}return true;
 }
 bool read(int fd){uint8_t buffer[65536];ssize_t n=recv(fd,buffer,sizeof(buffer),0);if(n>0){if(consumed){bytes.erase(bytes.begin(),bytes.begin()+consumed);consumed=0;}bytes.insert(bytes.end(),buffer,buffer+n);return true;}if(n==0)throw std::runtime_error("Browser transport closed");if(errno==EINTR)return true;if(errno==EAGAIN||errno==EWOULDBLOCK)return false;throw std::runtime_error("Browser transport read failed");}
 Frame wait(int fd){Frame frame;for(;;){if(next(frame))return frame;if(!read(fd)){pollfd item{fd,POLLIN,0};int result;do{result=poll(&item,1,-1);}while(result<0&&errno==EINTR);if(result<0)throw std::runtime_error("Browser transport wait failed");}}}
};
inline Value errorValue(const char*error){return Value(std::vector<uint8_t>(error,error+strlen(error)));}
inline std::string errorString(const Value&v){return std::string(reinterpret_cast<const char*>(v.bytesData()),v.bytesSize());}
}
