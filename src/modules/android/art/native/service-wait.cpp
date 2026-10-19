// Readiness uses the original Binder client without loading window, audio or
// resolver libraries into each short-lived query process.
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

int main(int argc,char**argv){
 if(argc<2)return 2;
 android::ProcessState::self()->startThreadPool();
 const auto manager=android::defaultServiceManager();
 if(!manager)return 15;
 std::atomic<bool> unavailable{false};
 const auto wait=[&](const char*name){
  if(!manager->waitForService(android::String16(name))){
   fprintf(stderr,"Android service unavailable: %s\n",name);
   unavailable=true;
  }
 };
 std::vector<std::thread> workers;
 for(int index=2;index<argc;++index)workers.emplace_back(wait,argv[index]);
 wait(argv[1]);
 for(auto&worker:workers)worker.join();
 return unavailable?15:0;
}
