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
// Complete pinned NdkMediaExtractor entrypoint family. The opaque handle is the
// same retained native owner used by Java MediaExtractor, not a wrapper table.
#include "MediaExtractorOwner.h"
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaExtractor.h>
#include <cerrno>
#include <climits>
#include <cstdint>

#ifndef EXPORT
#define EXPORT __attribute__((visibility("default")))
#endif

namespace {
KLJExtractor *owner(AMediaExtractor *value){return reinterpret_cast<KLJExtractor *>(value);}
media_status_t translated(int error) {
    // Darwin owner errors at the original NdkMediaErrorPriv::translate_error
    // boundary. That upstream function leaves BAD_VALUE/UNSUPPORTED as UNKNOWN.
    if(error==0)return AMEDIA_OK;
    if(error==1)return AMEDIA_ERROR_END_OF_STREAM;
    if(error==-EIO)return AMEDIA_ERROR_IO;
    if(error==-EBADMSG)return AMEDIA_ERROR_MALFORMED;
    if(error==-ENOSYS)return AMEDIA_ERROR_INVALID_OPERATION;
    return AMEDIA_ERROR_UNKNOWN;
}
}

extern "C" {
EXPORT AMediaExtractor *AMediaExtractor_new() {
    return reinterpret_cast<AMediaExtractor *>(klj_extractor_create());
}
EXPORT media_status_t AMediaExtractor_delete(AMediaExtractor *value) {
    if(value){klj_extractor_close(owner(value));klj_extractor_release(owner(value));}
    return AMEDIA_OK;
}
EXPORT media_status_t AMediaExtractor_setDataSourceFd(AMediaExtractor *value,int fd,off64_t offset,off64_t length) {
    return translated(klj_extractor_set_fd(owner(value),fd,offset,length));
}
EXPORT media_status_t AMediaExtractor_setDataSourceWithHeaders(AMediaExtractor *value,const char *uri,int count,const char *const *keys,const char *const *values) {
    if(count<0||(count&&(!keys||!values)))return AMEDIA_ERROR_INVALID_PARAMETER;
    for(int i=0;i<count;++i)if(!keys[i]||!values[i])return AMEDIA_ERROR_INVALID_PARAMETER;
    return translated(klj_extractor_set_path(owner(value),uri,keys,values,static_cast<size_t>(count)));
}
EXPORT media_status_t AMediaExtractor_setDataSource(AMediaExtractor *value,const char *location) {
    return AMediaExtractor_setDataSourceWithHeaders(value,location,0,nullptr,nullptr);
}
EXPORT media_status_t AMediaExtractor_setDataSourceCustom(AMediaExtractor *value,AMediaDataSource *source) {
    return translated(klj_extractor_set_source(owner(value),reinterpret_cast<klj_media_source *>(source)));
}
EXPORT AMediaFormat *AMediaExtractor_getFileFormat(AMediaExtractor *value) {
    AMediaFormat *result=klj_extractor_format(owner(value),-1);
    // Original AMediaFormat_fromMsg returns a real empty AMessage if absent.
    return result?result:AMediaFormat_new();
}
EXPORT size_t AMediaExtractor_getTrackCount(AMediaExtractor *value) {
    return klj_extractor_count(owner(value));
}
EXPORT AMediaFormat *AMediaExtractor_getTrackFormat(AMediaExtractor *value,size_t index) {
    AMediaFormat *result=index<=INT_MAX?klj_extractor_format(owner(value),static_cast<int>(index)):nullptr;
    return result?result:AMediaFormat_new();
}
EXPORT media_status_t AMediaExtractor_selectTrack(AMediaExtractor *value,size_t index) {
    return translated(klj_extractor_select(owner(value),index,true));
}
EXPORT media_status_t AMediaExtractor_unselectTrack(AMediaExtractor *value,size_t index) {
    return translated(klj_extractor_select(owner(value),index,false));
}
EXPORT bool AMediaExtractor_advance(AMediaExtractor *value) {
    return klj_extractor_advance(owner(value))>0;
}
EXPORT media_status_t AMediaExtractor_seekTo(AMediaExtractor *value,int64_t time,SeekMode mode) {
    // Pinned NDK behavior defaults other values to NEXT_SYNC; Java rejects them.
    int nativeMode=mode==AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC?0:mode==AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC?2:1;
    return translated(klj_extractor_seek(owner(value),time,nativeMode));
}
EXPORT ssize_t AMediaExtractor_readSampleData(AMediaExtractor *value,uint8_t *buffer,size_t capacity) {
    ssize_t result=klj_extractor_read(owner(value),buffer,capacity);
    return result>=0?result:-1;
}
EXPORT ssize_t AMediaExtractor_getSampleSize(AMediaExtractor *value) {
    int64_t result=klj_extractor_sample_size(owner(value));
    return result>=0&&static_cast<uint64_t>(result)<=SSIZE_MAX?static_cast<ssize_t>(result):-1;
}
EXPORT uint32_t AMediaExtractor_getSampleFlags(AMediaExtractor *value) {
    int result=klj_extractor_sample_flags(owner(value));
    return result>=0?static_cast<uint32_t>(result):UINT32_MAX;
}
EXPORT int AMediaExtractor_getSampleTrackIndex(AMediaExtractor *value) {
    int result=klj_extractor_sample_track(owner(value));return result>=0?result:-1;
}
EXPORT int64_t AMediaExtractor_getSampleTime(AMediaExtractor *value) {
    int64_t time=-1;return klj_extractor_sample_time_status(owner(value),&time)==0?time:-1;
}
EXPORT PsshInfo *AMediaExtractor_getPsshInfo(AMediaExtractor *) {
    // Shared source admission rejects AVAsset.hasProtectedContent. Its clear
    // sources contain no exposed PSSH table; no synthetic crypto state is made.
    return nullptr;
}
EXPORT AMediaCodecCryptoInfo *AMediaExtractor_getSampleCryptoInfo(AMediaExtractor *value) {
    // Resolve the current sample through the same owner. Accepted sources are
    // unencrypted; upstream likewise returns null without encrypted-size data.
    (void)klj_extractor_sample_flags(owner(value));
    return nullptr;
}
EXPORT int64_t AMediaExtractor_getCachedDuration(AMediaExtractor *value) {
    bool eos=true;int64_t duration=klj_extractor_cached_duration(owner(value),&eos);
    return duration>=0?duration:-1;
}
EXPORT media_status_t AMediaExtractor_getSampleFormat(AMediaExtractor *value,AMediaFormat *metadata) {
    if(!metadata)return AMEDIA_ERROR_INVALID_PARAMETER;
    return translated(klj_extractor_sample_format(owner(value),metadata));
}
} // extern "C"
