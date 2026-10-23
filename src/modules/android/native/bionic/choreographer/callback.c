/* Callbacks execute on the original Choreographer Looper thread. Descriptor
 * storage belongs to its native frame/refresh entry, including cancellation. */
static int64_t callback_packet(const KljChoreographerCallback *callback,KljCallPacket *packet){
    JNIEnv *environment=NULL;
    jint attached=(*owner.vm)->GetEnv(owner.vm,(void**)&environment,JNI_VERSION_1_6);
    if(!((attached==JNI_OK&&environment)||(attached==JNI_EDETACHED&&!environment)))return -22;
    int saved=errno;
    int64_t result=owner.invoke(owner.context,environment,callback->caller,packet);
    errno=saved;
    return result?result:packet->status;
}
static void frame_long_callback(long frame,void *raw){
    const KljChoreographerCallback *callback=raw;
    KljCallPacket packet={.size=sizeof(packet),.version=KLJ_CALL_PACKET_VERSION,.target=callback->function,
        .x={(uint64_t)(int64_t)frame,callback->user}};
    (void)callback_packet(callback,&packet);
}
static void frame64_callback(int64_t frame,void *raw){
    const KljChoreographerCallback *callback=raw;
    KljCallPacket packet={.size=sizeof(packet),.version=KLJ_CALL_PACKET_VERSION,.target=callback->function,
        .x={(uint64_t)frame,callback->user}};
    (void)callback_packet(callback,&packet);
}
static void vsync_callback(const AChoreographerFrameCallbackData *timing,void *raw){
    const KljChoreographerCallback *callback=raw;
    KljCallPacket packet={.size=sizeof(packet),.version=KLJ_CALL_PACKET_VERSION,.target=callback->function,
        .x={(uintptr_t)timing,callback->user}};
    (void)callback_packet(callback,&packet);
}
static void refresh_callback(int64_t period,void *raw){
    const KljChoreographerCallback *callback=raw;
    KljCallPacket packet={.size=sizeof(packet),.version=KLJ_CALL_PACKET_VERSION,.target=callback->function,
        .x={(uint64_t)period,callback->user}};
    (void)callback_packet(callback,&packet);
}
