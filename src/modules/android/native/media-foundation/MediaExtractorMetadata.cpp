#include <media/NdkMediaFormat.h>
#include <media/esds/ESDS.h>
#include <cstddef>
extern "C" void klj_extractor_esds(AMediaFormat *format,const void *bytes,size_t size){
    if(!format||!bytes||!size)return;
    android::ESDS descriptor(bytes,size);
    const void *specific=nullptr;size_t length=0;
    if(descriptor.InitCheck()==android::OK&&descriptor.getCodecSpecificInfo(&specific,&length)==android::OK&&length)
        AMediaFormat_setBuffer(format,"csd-0",const_cast<void*>(specific),length);
}
