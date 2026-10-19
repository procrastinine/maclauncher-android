#pragma once
#include <array>
#include <cstring>
#include <mutex>
#include <string>
namespace klj_target {
// Surface presentation targets are an explicit host API. Do not inherit
// ANGLE's pbuffer-only extension claim for the final display presenter.
inline const char* withoutExtension(const char*value,unsigned&error){
 constexpr char token[]="EGL_ANDROID_presentation_time";
 if(!value||!strstr(value,token))return value;
 constexpr size_t maximum=65536;size_t length=strnlen(value,maximum+1);
 if(length>maximum){error=0x3003;return nullptr;}
 struct Entry {std::string original,filtered;bool occupied=false;};
 static std::array<Entry,8>cache;static std::mutex mutex;
 std::lock_guard lock(mutex);
 for(auto&entry:cache)if(entry.occupied&&entry.original==value)return entry.filtered.c_str();
 for(auto&entry:cache)if(!entry.occupied){
  std::string filtered;filtered.reserve(length);
  for(size_t start=0;start<length;){size_t end=start;while(end<length&&value[end]!=' ')++end;
   if(end>start&&(end-start!=sizeof(token)-1||memcmp(value+start,token,sizeof(token)-1))){if(!filtered.empty())filtered+=' ';filtered.append(value+start,end-start);}start=end+1;
  }
  entry.original=value;entry.filtered=std::move(filtered);entry.occupied=true;return entry.filtered.c_str();
 }
 error=0x3003;return nullptr;
}
}
