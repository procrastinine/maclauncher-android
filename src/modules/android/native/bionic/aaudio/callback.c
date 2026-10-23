/* The backend owns these descriptors from stream open until its original
 * close has joined the feeder. No allocation or logging occurs per buffer. */
static int64_t callback_packet(const KljAAudioCallback *callback,KljCallPacket *packet){
    JNIEnv *environment=NULL;
    jint attached=(*owner.vm)->GetEnv(owner.vm,(void**)&environment,JNI_VERSION_1_6);
    if(!((attached==JNI_OK&&environment)||(attached==JNI_EDETACHED&&!environment)))return -22;
    int saved=errno;
    int64_t result=owner.invoke(owner.context,environment,callback->caller,packet);
    errno=saved;
    return result?result:packet->status;
}
static aaudio_data_callback_result_t data_callback(AAudioStream *stream,void *raw,void *audio,int32_t frames){
    const KljAAudioCallback *callback=raw;
    KljCallPacket packet={.size=sizeof(packet),.version=KLJ_CALL_PACKET_VERSION,.target=callback->function,
        .x={(uintptr_t)stream,callback->user,(uintptr_t)audio,(uint64_t)(int64_t)frames}};
    if(callback_packet(callback,&packet))return AAUDIO_CALLBACK_RESULT_STOP;
    return (aaudio_data_callback_result_t)packet.result_x[0];
}
static void error_callback(AAudioStream *stream,void *raw,aaudio_result_t error){
    const KljAAudioCallback *callback=raw;
    KljCallPacket packet={.size=sizeof(packet),.version=KLJ_CALL_PACKET_VERSION,.target=callback->function,
        .x={(uintptr_t)stream,callback->user,(uint64_t)(int64_t)error}};
    (void)callback_packet(callback,&packet);
}
static void presentation_callback(AAudioStream *stream,void *raw){
    const KljAAudioCallback *callback=raw;
    KljCallPacket packet={.size=sizeof(packet),.version=KLJ_CALL_PACKET_VERSION,.target=callback->function,
        .x={(uintptr_t)stream,callback->user}};
    (void)callback_packet(callback,&packet);
}
