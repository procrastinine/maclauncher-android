#include <sys/system_properties.h>
#include <time.h>

extern "C" void* kl_shim_lookup(const char*);

// Every native library uses the one existing process property owner. There is
// no property area, cached value, serial, or handle state in this forwarding unit.
#define FORWARD(result,name,parameters,arguments) \
extern "C" result name parameters { \
    static const auto call=reinterpret_cast<decltype(&name)>(kl_shim_lookup(#name)); \
    return call arguments; \
}
FORWARD(int,__system_property_get,(const char* name,char* value),(name,value))
FORWARD(int,__system_property_set,(const char* name,const char* value),(name,value))
FORWARD(const prop_info*,__system_property_find,(const char* name),(name))
FORWARD(int,__system_property_read,(const prop_info* info,char* name,char* value),(info,name,value))
FORWARD(void,__system_property_read_callback,(const prop_info* info,void (*callback)(void*,const char*,const char*,uint32_t),void* cookie),(info,callback,cookie))
FORWARD(int,__system_property_foreach,(void (*callback)(const prop_info*,void*),void* cookie),(callback,cookie))
FORWARD(const prop_info*,__system_property_find_nth,(unsigned index),(index))
FORWARD(uint32_t,__system_property_serial,(const prop_info* info),(info))
FORWARD(uint32_t,__system_property_area_serial,(void),())
FORWARD(bool,__system_property_wait,(const prop_info* info,uint32_t previous,uint32_t* next,const struct timespec* timeout),(info,previous,next,timeout))
FORWARD(uint32_t,__system_property_wait_any,(uint32_t previous),(previous))
#undef FORWARD
