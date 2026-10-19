#pragma once
// Android RGB window configurations use ANGLE's existing IOSurface RGBX
// attachment implementation. The original RGBA configurations remain available.
struct OpaqueConfig {void *display,*raw;int id;};
std::mutex configsMutex;
std::unordered_map<void*,std::shared_ptr<OpaqueConfig>> opaqueConfigs;
std::shared_ptr<OpaqueConfig> opaqueConfig(void *config){std::lock_guard lock(configsMutex);auto at=opaqueConfigs.find(config);return at==opaqueConfigs.end()?nullptr:at->second;}
void *configRaw(void *config){auto owner=opaqueConfig(config);return owner?owner->raw:config;}
std::vector<void*> configList(void*d,bool &okay){
 int count=0;okay=fn<unsigned(*)(void*,void**,int,int*)>("eglGetConfigs")(d,nullptr,0,&count);if(!okay)return {};
 std::vector<void*> raw(count);okay=fn<unsigned(*)(void*,void**,int,int*)>("eglGetConfigs")(d,raw.data(),count,&count);if(!okay)return {};raw.resize(count);
 auto get=fn<unsigned(*)(void*,void*,int,int*)>("eglGetConfigAttrib");int maximum=0;for(auto config:raw){int id=0;get(d,config,EGL_CONFIG_ID,&id);maximum=std::max(maximum,id);}
 std::vector<void*> all;all.reserve(raw.size()*2);
 for(size_t i=0;i<raw.size();++i){auto config=raw[i];int red=0,green=0,blue=0,alpha=0,type=0;
  get(d,config,EGL_RED_SIZE,&red);get(d,config,EGL_GREEN_SIZE,&green);get(d,config,EGL_BLUE_SIZE,&blue);get(d,config,EGL_ALPHA_SIZE,&alpha);get(d,config,EGL_SURFACE_TYPE,&type);
  if(red==8&&green==8&&blue==8&&alpha==8&&(type&EGL_PBUFFER_BIT)){
   std::lock_guard lock(configsMutex);void*identity=nullptr;
   for(auto &[key,value]:opaqueConfigs)if(value->display==d&&value->raw==config){identity=key;break;}
   if(!identity){auto value=std::make_shared<OpaqueConfig>(OpaqueConfig{d,config,maximum+int(i)+1});identity=value.get();opaqueConfigs.emplace(identity,std::move(value));}
   all.push_back(identity);
  }
  all.push_back(config);
 }
 return all;
}
unsigned configs(void*d,void**output,int capacity,int*count){
 if(!count||capacity<0){error=EGL_BAD_PARAMETER;return 0;}bool okay=false;auto all=configList(d,okay);if(!okay)return 0;
 *count=output?std::min(capacity,int(all.size())):int(all.size());if(output)std::copy_n(all.begin(),*count,output);return 1;
}
unsigned attribute(void*d,void*config,int name,int*value){
 auto owner=opaqueConfig(config);auto raw=owner?owner->raw:config;
 if(owner&&owner->display!=d){error=EGL_BAD_CONFIG;return 0;}
 auto okay=fn<unsigned(*)(void*,void*,int,int*)>("eglGetConfigAttrib")(d,raw,name,value);if(!okay)return okay;
 if(name==EGL_SURFACE_TYPE&&(*value&EGL_PBUFFER_BIT))*value|=EGL_WINDOW_BIT;
 if(name==EGL_NATIVE_VISUAL_ID&&*value==0){int red=0,green=0,blue=0,alpha=0;auto get=fn<unsigned(*)(void*,void*,int,int*)>("eglGetConfigAttrib");get(d,raw,EGL_RED_SIZE,&red);get(d,raw,EGL_GREEN_SIZE,&green);get(d,raw,EGL_BLUE_SIZE,&blue);get(d,raw,EGL_ALPHA_SIZE,&alpha);if(red==8&&green==8&&blue==8)*value=alpha?1:2;else if(red==5&&green==6&&blue==5&&!alpha)*value=4;}
 if(owner)switch(name){case EGL_ALPHA_SIZE:*value=0;break;case EGL_BUFFER_SIZE:*value-=8;break;case EGL_CONFIG_ID:*value=owner->id;break;case EGL_SURFACE_TYPE:*value=EGL_WINDOW_BIT;break;case EGL_NATIVE_VISUAL_ID:*value=2;break;}
 return okay;
}
unsigned choose(void*d,const int*attributes,void**output,int capacity,int*count){
 if(!count||capacity<0){error=EGL_BAD_PARAMETER;return 0;}
 bool okay=false;auto all=configList(d,okay);if(!okay)return 0;
 std::vector<int> copy;int requestedId=EGL_DONT_CARE,minAlpha=0,minBuffer=0,surface=EGL_WINDOW_BIT,visual=EGL_DONT_CARE;bool hasSurfaceType=false;std::shared_ptr<OpaqueConfig> requestedOwner;
 if(attributes)for(size_t i=0;attributes[i]!=EGL_NONE;i+=2){int key=attributes[i],value=attributes[i+1];
  if(key==EGL_CONFIG_ID){requestedId=value;for(auto candidate:all){auto owner=opaqueConfig(candidate);if(owner&&owner->id==value){requestedOwner=owner;int rawId=0;attribute(d,owner->raw,EGL_CONFIG_ID,&rawId);value=rawId;break;}}}
  if(key==EGL_ALPHA_SIZE)minAlpha=value;if(key==EGL_BUFFER_SIZE)minBuffer=value;if(key==EGL_NATIVE_VISUAL_ID){visual=value;if(value==1||value==2||value==4)continue;}
  if(key==EGL_SURFACE_TYPE){hasSurfaceType=true;surface=value;if(value!=EGL_DONT_CARE&&(value&EGL_WINDOW_BIT))value=(value&~EGL_WINDOW_BIT)|EGL_PBUFFER_BIT;}
  copy.push_back(key);copy.push_back(value);
 }
 // EGL's omitted default is WINDOW_BIT too. Every advertised Android window
 // uses a provider IOSurface pbuffer, including native RGB565 configurations.
 if(!hasSurfaceType){copy.push_back(EGL_SURFACE_TYPE);copy.push_back(EGL_PBUFFER_BIT);}
 copy.push_back(EGL_NONE);
 int nativeCount=0;auto select=fn<unsigned(*)(void*,const int*,void**,int,int*)>("eglChooseConfig");
 if(!select(d,copy.data(),nullptr,0,&nativeCount))return 0;
 std::vector<void*> selected(nativeCount);if(!select(d,copy.data(),selected.data(),nativeCount,&nativeCount))return 0;selected.resize(nativeCount);
 std::vector<void*> result;
 for(auto raw:selected){
  if(requestedOwner||((minAlpha==EGL_DONT_CARE||minAlpha==0)&&(minBuffer==EGL_DONT_CARE||minBuffer<=24)&&(surface==EGL_DONT_CARE||(surface&~EGL_WINDOW_BIT)==0)&&(visual==EGL_DONT_CARE||visual==2))){
   for(auto candidate:all){auto owner=opaqueConfig(candidate);if(owner&&owner->raw==raw&&(requestedId==EGL_DONT_CARE||requestedId==owner->id)){result.push_back(candidate);break;}}
  }
  if(!requestedOwner){int nativeVisual=0;if(visual==EGL_DONT_CARE||!attribute(d,raw,EGL_NATIVE_VISUAL_ID,&nativeVisual)||nativeVisual==visual)result.push_back(raw);}
 }
 *count=output?std::min(capacity,int(result.size())):int(result.size());if(output)std::copy_n(result.begin(),*count,output);return 1;
}
