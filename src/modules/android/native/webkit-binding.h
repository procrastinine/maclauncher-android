#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace klj::web {
enum class Op:uint32_t {initialize,create,destroy,visibility,focus,size,load,html,script,navigate,reload,stop,snapshot,touch,key,edit,setting,interfaces,dialogResult,state,restore,command,cookies,cookieCommand,storageCommand,editor};
enum class Event:uint32_t {changed,dimensions,navigation,history,dialog,resource,editor,port,navigationRequest,interface,script,frame,result};
// Only callbacks with a return value hold the physical operation open. Void
// notifications remain ordered on Android's owner without blocking AppKit.
inline bool requiresReply(Event event){return event==Event::resource||event==Event::navigationRequest||event==Event::interface;}
struct Value {
    enum Type:uint8_t {null,integer,real,text,bytes,list} type=null;
    int64_t number=0;double decimal=0;
    std::u16string string;std::vector<uint8_t> data;std::vector<Value> items;
    const uint8_t* borrowed=nullptr;size_t borrowedSize=0;
    Value()=default;
    template<class T,std::enable_if_t<std::is_integral_v<T>,int> =0> Value(T n):type(integer),number(n){}
    Value(float n):type(real),decimal(n){}
    explicit Value(std::u16string s):type(text),string(std::move(s)){}
    explicit Value(std::vector<uint8_t> b):type(bytes),data(std::move(b)){}
    explicit Value(std::vector<Value> v):type(list),items(std::move(v)){}
    static Value view(const void*p,size_t size){Value v;v.type=bytes;v.borrowed=static_cast<const uint8_t*>(p);v.borrowedSize=size;return v;}
    const uint8_t* bytesData()const{return borrowed?borrowed:data.data();}
    size_t bytesSize()const{return borrowed?borrowedSize:data.size();}
};
using Args=std::vector<Value>;
struct Callback:std::enable_shared_from_this<Callback> {virtual ~Callback()=default;virtual Value invoke(Event event,const Args& arguments)=0;};
// Direct in-process calls and the original ART window transport implement the
// same operation/callback interface. Java/ART references never cross it.
Value request(void* vmThread,Op operation,const Args& arguments,std::shared_ptr<Callback> callback={});

// Length-delimited values preserve null, UTF-16, binary resources and 64-bit
// identities. This codec carries only this boundary's actual argument types.
inline void append(std::vector<uint8_t>&out,const void*data,size_t size){if(!size)return;const auto*p=static_cast<const uint8_t*>(data);out.insert(out.end(),p,p+size);}
inline void encode(std::vector<uint8_t>&out,const Value&v){
    out.push_back(v.type);uint64_t length=0;
    switch(v.type){
    case Value::null:return;
    case Value::integer:append(out,&v.number,8);return;
    case Value::real:append(out,&v.decimal,8);return;
    case Value::text:length=v.string.size();append(out,&length,8);append(out,v.string.data(),v.string.size()*2);return;
    case Value::bytes:length=v.bytesSize();append(out,&length,8);append(out,v.bytesData(),length);return;
    case Value::list:length=v.items.size();append(out,&length,8);for(const auto&item:v.items)encode(out,item);return;
    }throw std::runtime_error("Invalid browser value type");
}
struct Reader {
    const uint8_t*data;size_t remaining;
    void take(void*out,size_t n){if(n>remaining)throw std::runtime_error("Incomplete browser value");if(n)memcpy(out,data,n);data+=n;remaining-=n;}
    Value value(unsigned depth=0){if(depth>2)throw std::runtime_error("Invalid nested browser argument");uint8_t tag;take(&tag,1);Value v;v.type=static_cast<Value::Type>(tag);uint64_t n;
        switch(v.type){
        case Value::null:return v;
        case Value::integer:take(&v.number,8);return v;
        case Value::real:take(&v.decimal,8);return v;
        case Value::text:take(&n,8);if(n>remaining/2)throw std::runtime_error("Invalid browser string length");v.string.resize(n);take(v.string.data(),n*2);return v;
        case Value::bytes:take(&n,8);if(n>remaining)throw std::runtime_error("Invalid browser data length");v.data.resize(n);take(v.data.data(),n);return v;
        case Value::list:take(&n,8);if(n>remaining)throw std::runtime_error("Invalid browser argument count");v.items.reserve(n);for(uint64_t i=0;i<n;i++)v.items.push_back(value(depth+1));return v;
        }throw std::runtime_error("Invalid browser value type");
    }
};
inline std::vector<uint8_t> encode(const Value&v){std::vector<uint8_t>out;encode(out,v);return out;}
inline Value decode(const void*data,size_t length){Reader r{static_cast<const uint8_t*>(data),length};auto v=r.value();if(r.remaining)throw std::runtime_error("Trailing browser value bytes");return v;}
}
