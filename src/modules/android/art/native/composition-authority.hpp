#pragma once
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>

namespace klj_composition {
// The inherited launcher pipe carries the same six authenticated fields that
// the original command-line form accepts. Loading the process needs none of
// them; consuming them still follows the actual enrollment acknowledgement.
inline bool readBytes(int descriptor,void *output,size_t size){
 auto *bytes=static_cast<char*>(output);
 while(size){const auto count=read(descriptor,bytes,size);if(count<0&&errno==EINTR)continue;if(count<=0)return false;bytes+=count;size-=static_cast<size_t>(count);}
 return true;
}
inline bool readAuthority(int descriptor,std::array<std::string,6>&fields){
 uint32_t framed=0;if(!readBytes(descriptor,&framed,sizeof(framed)))return false;
 const auto size=ntohl(framed);if(!size||size>1024*1024)return false;
 std::vector<char> bytes(size);if(!readBytes(descriptor,bytes.data(),size))return false;
 size_t offset=0;
 for(auto &field:fields){
  const auto *end=static_cast<const char*>(memchr(bytes.data()+offset,0,size-offset));
  if(!end||end==bytes.data()+offset)return false;
  field.assign(bytes.data()+offset,static_cast<size_t>(end-(bytes.data()+offset)));offset=static_cast<size_t>(end-bytes.data())+1;
 }
 return offset==size;
}
}
