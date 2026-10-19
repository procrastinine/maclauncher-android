#pragma once
#include "webkit-platform.h"
#include "webkit-binding.h"
namespace klj::web {
Value physicalRequest(std::shared_ptr<KljWebContext> context,Op operation,const Args& arguments,std::shared_ptr<Callback> callback,void* contentView);
}
