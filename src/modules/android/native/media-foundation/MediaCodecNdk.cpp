/*
 * Copyright (C) 2014 The Android Open Source Project
 * Copyright (C) 2026 MacLauncher contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "NdkMediaCodec"
#include "MediaCodecOwner.h"
#include "process-identity.h"
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaCodec.h>
#include <log/log.h>
#include <atomic>
#include <bit>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unistd.h>

#ifndef EXPORT
#define EXPORT __attribute__((visibility("default")))
#endif
namespace {
// AMediaCodec is the shared Java/NDK native owner itself, never a wrapper table.
KLJCodec *owner(AMediaCodec *value){return reinterpret_cast<KLJCodec*>(value);}
struct Call {
    KLJCodec *value;
    explicit Call(AMediaCodec *codec):value(owner(codec)){klj_codec_retain(value);}
    ~Call(){klj_codec_release(value);}
};
template<class F> media_status_t status(F operation){
    try{return static_cast<media_status_t>(operation());}
    catch(const std::bad_alloc&){return AMEDIACODEC_ERROR_INSUFFICIENT_RESOURCE;}
    catch(...){return AMEDIA_ERROR_UNKNOWN;}
}
template<class F> auto pointer(F operation)->decltype(operation()){
    try{return operation();}catch(const std::bad_alloc&){errno=ENOMEM;return nullptr;}
    catch(...){errno=EIO;return nullptr;}
}
AMediaCodec *create(const char *name,bool type,bool encoder){
    if(!name){errno=EINVAL;return nullptr;}
    return pointer([&]{return reinterpret_cast<AMediaCodec*>(klj_codec_create(name,type,encoder));});
}
bool selfClient(pid_t pid,uid_t uid){
    if((pid!=-1&&pid!=getpid())||(uid!=static_cast<uid_t>(-1)&&uid!=klj_process_uid())){errno=EPERM;return false;}
    return true;
}
struct Surface {
    KLJCodecSurface value={};
    ~Surface(){if(value.release)value.release(value.context);}
};
enum { ACTION_CODE_FATAL, ACTION_CODE_TRANSIENT, ACTION_CODE_RECOVERABLE };

struct Async {
    std::atomic<unsigned> references{1};
    AMediaCodec *codec;
    AMediaCodecOnAsyncNotifyCallback callbacks;
    void *userdata;
    Async(AMediaCodec*c,AMediaCodecOnAsyncNotifyCallback cb,void*u):codec(c),callbacks(cb),userdata(u){}
    static void retain(void*p){static_cast<Async*>(p)->references.fetch_add(1,std::memory_order_relaxed);}
    static void release(void*p){auto*s=static_cast<Async*>(p);if(s->references.fetch_sub(1,std::memory_order_acq_rel)==1)delete s;}
    static void error(void*p,int value){auto&s=*static_cast<Async*>(p);if(s.callbacks.onAsyncError)s.callbacks.onAsyncError(s.codec,s.userdata,static_cast<media_status_t>(value),ACTION_CODE_FATAL,"Native decoder error");}
    static void input(void*p,size_t index){auto&s=*static_cast<Async*>(p);if(index>INT32_MAX){error(p,AMEDIA_ERROR_INVALID_PARAMETER);return;}if(s.callbacks.onAsyncInputAvailable)s.callbacks.onAsyncInputAvailable(s.codec,s.userdata,static_cast<int32_t>(index));}
    static void output(void*p,size_t index,const KLJCodecBufferInfo*value){
        auto&s=*static_cast<Async*>(p);if(index>INT32_MAX||!value){error(p,AMEDIA_ERROR_INVALID_PARAMETER);return;}
        AMediaCodecBufferInfo info={value->offset,value->size,value->presentationTimeUs,value->flags};
        if(s.callbacks.onAsyncOutputAvailable)s.callbacks.onAsyncOutputAvailable(s.codec,s.userdata,static_cast<int32_t>(index),&info);
    }
    static void format(void*p,const AMediaFormat*value){
        auto&s=*static_cast<Async*>(p);if(!s.callbacks.onAsyncFormatChanged)return;
        AMediaFormat *copy=AMediaFormat_new();
        if(!copy){error(p,AMEDIACODEC_ERROR_INSUFFICIENT_RESOURCE);return;}
        media_status_t copied=AMediaFormat_copy(copy,const_cast<AMediaFormat*>(value));
        if(copied!=AMEDIA_OK){AMediaFormat_delete(copy);error(p,copied);return;}
        // Original NDK CodecHandler transfers its newly allocated format to
        // the callback; caller mutations must not alter the owner's snapshot.
        s.callbacks.onAsyncFormatChanged(s.codec,s.userdata,copy);
    }
};
struct Rendered {
    std::atomic<unsigned> references{1};
    AMediaCodec *codec;AMediaCodecOnFrameRendered callback;void *userdata;
    Rendered(AMediaCodec*c,AMediaCodecOnFrameRendered cb,void*u):codec(c),callback(cb),userdata(u){}
    static void retain(void*p){static_cast<Rendered*>(p)->references.fetch_add(1,std::memory_order_relaxed);}
    static void release(void*p){auto*s=static_cast<Rendered*>(p);if(s->references.fetch_sub(1,std::memory_order_acq_rel)==1)delete s;}
    static void rendered(void*p,int64_t media,int64_t system){auto&s=*static_cast<Rendered*>(p);s.callback(s.codec,s.userdata,media,system);}
};
}

extern "C" {
EXPORT AMediaCodec *AMediaCodec_createCodecByName(const char *name){return create(name,false,false);}
EXPORT AMediaCodec *AMediaCodec_createDecoderByType(const char *name){return create(name,true,false);}
EXPORT AMediaCodec *AMediaCodec_createEncoderByType(const char *name){return create(name,true,true);}
EXPORT AMediaCodec *AMediaCodec_createCodecByNameForClient(const char *name,pid_t pid,uid_t uid){return selfClient(pid,uid)?create(name,false,false):nullptr;}
EXPORT AMediaCodec *AMediaCodec_createDecoderByTypeForClient(const char *name,pid_t pid,uid_t uid){return selfClient(pid,uid)?create(name,true,false):nullptr;}
EXPORT AMediaCodec *AMediaCodec_createEncoderByTypeForClient(const char *name,pid_t pid,uid_t uid){return selfClient(pid,uid)?create(name,true,true):nullptr;}
EXPORT media_status_t AMediaCodec_delete(AMediaCodec *value){
    if(!value)return AMEDIA_OK;
    media_status_t result=status([&]{return klj_codec_close(owner(value));});
    klj_codec_release(owner(value));return result;
}
EXPORT media_status_t AMediaCodec_configure(AMediaCodec *value,const AMediaFormat *format,ANativeWindow *window,AMediaCrypto *crypto,uint32_t flags){
    if(!value||!format)return AMEDIA_ERROR_INVALID_PARAMETER;
    if(crypto)return AMEDIA_ERROR_UNSUPPORTED;
    Call call(value);Surface surface;
    return status([&]{if(window){int result=klj_media_surface_acquire(window,&surface.value);if(result)return result;}
        return klj_codec_configure(call.value,format,window?&surface.value:nullptr,flags);});
}
EXPORT media_status_t AMediaCodec_start(AMediaCodec *value){Call call(value);return status([&]{return klj_codec_start(call.value);});}
EXPORT media_status_t AMediaCodec_stop(AMediaCodec *value){Call call(value);return status([&]{return klj_codec_stop(call.value);});}
EXPORT media_status_t AMediaCodec_flush(AMediaCodec *value){Call call(value);return status([&]{return klj_codec_flush(call.value);});}
EXPORT uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *value,size_t index,size_t *size){Call call(value);return pointer([&]{return klj_codec_input_buffer(call.value,index,size);});}
EXPORT uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *value,size_t index,size_t *size){Call call(value);return pointer([&]{return klj_codec_output_buffer(call.value,index,size);});}
EXPORT ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *value,int64_t timeout){
    Call call(value);try{return klj_codec_dequeue_input(call.value,timeout);}catch(const std::bad_alloc&){return AMEDIACODEC_ERROR_INSUFFICIENT_RESOURCE;}catch(...){return AMEDIA_ERROR_UNKNOWN;}
}
EXPORT media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *value,size_t index,off_t offset,size_t size,uint64_t time,uint32_t flags){
    Call call(value);return status([&]{return klj_codec_queue_input(call.value,index,offset,size,std::bit_cast<int64_t>(time),flags);});
}
EXPORT media_status_t AMediaCodec_queueSecureInputBuffer(AMediaCodec *value,size_t,off_t,AMediaCodecCryptoInfo *crypto,uint64_t,uint32_t){
    return !value||!crypto?AMEDIA_ERROR_INVALID_PARAMETER:AMEDIA_ERROR_UNSUPPORTED;
}
EXPORT ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *value,AMediaCodecBufferInfo *info,int64_t timeout){
    if(!info)return AMEDIA_ERROR_INVALID_PARAMETER;Call call(value);KLJCodecBufferInfo native={};
    try{ssize_t result=klj_codec_dequeue_output(call.value,&native,timeout);if(result>=0)*info={0,native.size,native.presentationTimeUs,native.flags};return result;}
    catch(const std::bad_alloc&){return AMEDIACODEC_ERROR_INSUFFICIENT_RESOURCE;}catch(...){return AMEDIA_ERROR_UNKNOWN;}
}
EXPORT AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *value){Call call(value);return pointer([&]{auto*f=klj_codec_format(call.value,false);return f?f:AMediaFormat_new();});}
EXPORT AMediaFormat *AMediaCodec_getInputFormat(AMediaCodec *value){Call call(value);return pointer([&]{auto*f=klj_codec_format(call.value,true);return f?f:AMediaFormat_new();});}
EXPORT AMediaFormat *AMediaCodec_getBufferFormat(AMediaCodec *value,size_t index){Call call(value);return pointer([&]{auto*f=klj_codec_buffer_format(call.value,index);return f?f:AMediaFormat_new();});}
EXPORT media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *value,size_t index,bool render){Call call(value);return status([&]{return klj_codec_release_output(call.value,index,render,-1);});}
EXPORT media_status_t AMediaCodec_releaseOutputBufferAtTime(AMediaCodec *value,size_t index,int64_t time){Call call(value);return status([&]{return klj_codec_release_output(call.value,index,true,time);});}
EXPORT media_status_t AMediaCodec_setOutputSurface(AMediaCodec *value,ANativeWindow *window){
    Call call(value);Surface surface;return status([&]{if(window){int result=klj_media_surface_acquire(window,&surface.value);if(result)return result;}return klj_codec_set_surface(call.value,window?&surface.value:nullptr);});
}
EXPORT media_status_t AMediaCodec_createInputSurface(AMediaCodec *value,ANativeWindow **window){if(!value||!window)return AMEDIA_ERROR_INVALID_PARAMETER;*window=nullptr;return AMEDIA_ERROR_UNSUPPORTED;}
EXPORT media_status_t AMediaCodec_createPersistentInputSurface(ANativeWindow **window){if(!window)return AMEDIA_ERROR_INVALID_PARAMETER;*window=nullptr;return AMEDIA_ERROR_UNSUPPORTED;}
EXPORT media_status_t AMediaCodec_setInputSurface(AMediaCodec *value,ANativeWindow *window){return !value||!window?AMEDIA_ERROR_INVALID_PARAMETER:AMEDIA_ERROR_UNSUPPORTED;}
EXPORT media_status_t AMediaCodec_setParameters(AMediaCodec *value,const AMediaFormat *format){if(!value||!format)return AMEDIA_ERROR_INVALID_PARAMETER;Call call(value);return status([&]{return klj_codec_parameters(call.value,format);});}
EXPORT media_status_t AMediaCodec_signalEndOfInputStream(AMediaCodec *value){return value?AMEDIA_ERROR_INVALID_OPERATION:AMEDIA_ERROR_INVALID_PARAMETER;}
EXPORT media_status_t AMediaCodec_getName(AMediaCodec *value,char **name){
    if(!name)return AMEDIA_ERROR_INVALID_PARAMETER;*name=nullptr;Call call(value);
    const char *native=klj_codec_name(call.value);if(!native)return AMEDIA_ERROR_INVALID_OBJECT;
    *name=strdup(native);return *name?AMEDIA_OK:AMEDIACODEC_ERROR_INSUFFICIENT_RESOURCE;
}
EXPORT void AMediaCodec_releaseName(AMediaCodec*,char *name){free(name);}
EXPORT media_status_t AMediaCodec_setAsyncNotifyCallback(AMediaCodec *value,AMediaCodecOnAsyncNotifyCallback callbacks,void *userdata){
    if(!value)return AMEDIA_ERROR_INVALID_PARAMETER;Call call(value);
    if(!callbacks.onAsyncInputAvailable&&!callbacks.onAsyncOutputAvailable&&!callbacks.onAsyncFormatChanged&&!callbacks.onAsyncError)return status([&]{return klj_codec_callbacks(call.value,nullptr);});
    auto*state=new(std::nothrow) Async(value,callbacks,userdata);if(!state)return AMEDIACODEC_ERROR_INSUFFICIENT_RESOURCE;
    KLJCodecCallbacks bridge={state,Async::retain,Async::release,Async::input,Async::output,Async::format,Async::error};
    media_status_t result=status([&]{return klj_codec_callbacks(call.value,&bridge);});Async::release(state);return result;
}
EXPORT media_status_t AMediaCodec_setOnFrameRenderedCallback(AMediaCodec *value,AMediaCodecOnFrameRendered callback,void *userdata){
    if(!value)return AMEDIA_ERROR_INVALID_PARAMETER;Call call(value);
    if(!callback)return status([&]{return klj_codec_rendered_callbacks(call.value,nullptr);});
    auto*state=new(std::nothrow) Rendered(value,callback,userdata);if(!state)return AMEDIACODEC_ERROR_INSUFFICIENT_RESOURCE;
    KLJCodecRenderedCallbacks bridge={state,Rendered::retain,Rendered::release,Rendered::rendered};
    media_status_t result=status([&]{return klj_codec_rendered_callbacks(call.value,&bridge);});Rendered::release(state);return result;
}
EXPORT media_status_t AMediaCodec_releaseCrypto(AMediaCodec *value){return value?AMEDIA_ERROR_INVALID_OPERATION:AMEDIA_ERROR_INVALID_PARAMETER;}

// The remaining metadata helpers and constants are the complete original
// NdkMediaCodec.cpp family, independent of decoder/DRM platform support.

typedef struct AMediaCodecCryptoInfo {
        int numsubsamples;
        uint8_t key[16];
        uint8_t iv[16];
        cryptoinfo_mode_t mode;
        cryptoinfo_pattern_t pattern;
        size_t *clearbytes;
        size_t *encryptedbytes;
} AMediaCodecCryptoInfo;

EXPORT
bool AMediaCodecActionCode_isRecoverable(int32_t actionCode) {
    return (actionCode == ACTION_CODE_RECOVERABLE);
}

EXPORT
bool AMediaCodecActionCode_isTransient(int32_t actionCode) {
    return (actionCode == ACTION_CODE_TRANSIENT);
}


EXPORT
void AMediaCodecCryptoInfo_setPattern(AMediaCodecCryptoInfo *info,
        cryptoinfo_pattern_t *pattern) {
    info->pattern.encryptBlocks = pattern->encryptBlocks;
    info->pattern.skipBlocks = pattern->skipBlocks;
}

EXPORT
AMediaCodecCryptoInfo *AMediaCodecCryptoInfo_new(
        int numsubsamples,
        uint8_t key[16],
        uint8_t iv[16],
        cryptoinfo_mode_t mode,
        size_t *clearbytes,
        size_t *encryptedbytes) {

    // size needed to store all the crypto data
    size_t cryptosize;
    // = sizeof(AMediaCodecCryptoInfo) + sizeof(size_t) * numsubsamples * 2;
    if (__builtin_mul_overflow(sizeof(size_t) * 2, numsubsamples, &cryptosize) ||
            __builtin_add_overflow(cryptosize, sizeof(AMediaCodecCryptoInfo), &cryptosize)) {
        ALOGE("crypto size overflow");
        return NULL;
    }
    AMediaCodecCryptoInfo *ret = (AMediaCodecCryptoInfo*) malloc(cryptosize);
    if (!ret) {
        ALOGE("couldn't allocate %zu bytes", cryptosize);
        return NULL;
    }
    ret->numsubsamples = numsubsamples;
    memcpy(ret->key, key, 16);
    memcpy(ret->iv, iv, 16);
    ret->mode = mode;
    ret->pattern.encryptBlocks = 0;
    ret->pattern.skipBlocks = 0;

    // clearbytes and encryptedbytes point at the actual data, which follows
    ret->clearbytes = (size_t*) (ret + 1); // point immediately after the struct
    ret->encryptedbytes = ret->clearbytes + numsubsamples; // point after the clear sizes

    memcpy(ret->clearbytes, clearbytes, numsubsamples * sizeof(size_t));
    memcpy(ret->encryptedbytes, encryptedbytes, numsubsamples * sizeof(size_t));

    return ret;
}


EXPORT
media_status_t AMediaCodecCryptoInfo_delete(AMediaCodecCryptoInfo* info) {
    free(info);
    return AMEDIA_OK;
}

EXPORT
size_t AMediaCodecCryptoInfo_getNumSubSamples(AMediaCodecCryptoInfo* ci) {
    return ci->numsubsamples;
}

EXPORT
media_status_t AMediaCodecCryptoInfo_getKey(AMediaCodecCryptoInfo* ci, uint8_t *dst) {
    if (!ci) {
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    if (!dst) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    memcpy(dst, ci->key, 16);
    return AMEDIA_OK;
}

EXPORT
media_status_t AMediaCodecCryptoInfo_getIV(AMediaCodecCryptoInfo* ci, uint8_t *dst) {
    if (!ci) {
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    if (!dst) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    memcpy(dst, ci->iv, 16);
    return AMEDIA_OK;
}

EXPORT
cryptoinfo_mode_t AMediaCodecCryptoInfo_getMode(AMediaCodecCryptoInfo* ci) {
    if (!ci) {
        return (cryptoinfo_mode_t) AMEDIA_ERROR_INVALID_OBJECT;
    }
    return ci->mode;
}

EXPORT
media_status_t AMediaCodecCryptoInfo_getClearBytes(AMediaCodecCryptoInfo* ci, size_t *dst) {
    if (!ci) {
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    if (!dst) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    memcpy(dst, ci->clearbytes, sizeof(size_t) * ci->numsubsamples);
    return AMEDIA_OK;
}

EXPORT
media_status_t AMediaCodecCryptoInfo_getEncryptedBytes(AMediaCodecCryptoInfo* ci, size_t *dst) {
    if (!ci) {
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    if (!dst) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    memcpy(dst, ci->encryptedbytes, sizeof(size_t) * ci->numsubsamples);
    return AMEDIA_OK;
}

EXPORT const char* AMEDIACODEC_KEY_HDR10_PLUS_INFO = AMEDIAFORMAT_KEY_HDR10_PLUS_INFO;
EXPORT const char* AMEDIACODEC_KEY_LOW_LATENCY = AMEDIAFORMAT_KEY_LOW_LATENCY;
EXPORT const char* AMEDIACODEC_KEY_OFFSET_TIME = "time-offset-us";
EXPORT const char* AMEDIACODEC_KEY_REQUEST_SYNC_FRAME = "request-sync";
EXPORT const char* AMEDIACODEC_KEY_SUSPEND = "drop-input-frames";
EXPORT const char* AMEDIACODEC_KEY_SUSPEND_TIME = "drop-start-time-us";
EXPORT const char* AMEDIACODEC_KEY_VIDEO_BITRATE = "video-bitrate";

} // extern "C"

