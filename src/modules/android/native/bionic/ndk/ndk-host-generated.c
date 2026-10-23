#include "ndk-arguments.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
extern int kl_errno_to_linux(int),kl_errno_from_linux(int);
static KljBionicNdkOwner owner;static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;static _Atomic int ready;
static void (*target_0)(AAssetDir *);
static int64_t invoke_0(JNIEnv *env,KljBionicNdkPacket *packet){Args_0 *a=(void*)(uintptr_t)packet->arguments;target_0(a->a0);return 0;}
static const char * (*target_1)(AAssetDir *);
static int64_t invoke_1(JNIEnv *env,KljBionicNdkPacket *packet){Args_1 *a=(void*)(uintptr_t)packet->arguments;a->result=target_1(a->a0);return 0;}
static void (*target_2)(AAssetDir *);
static int64_t invoke_2(JNIEnv *env,KljBionicNdkPacket *packet){Args_2 *a=(void*)(uintptr_t)packet->arguments;target_2(a->a0);return 0;}
static AAssetManager * (*target_3)(JNIEnv *,jobject);
static int64_t invoke_3(JNIEnv *env,KljBionicNdkPacket *packet){Args_3 *a=(void*)(uintptr_t)packet->arguments;if(!env)return -22;a->a0=owner.environment(owner.context,env,a->a0);if(!a->a0||a->a0!=env){return -22;}a->result=target_3(a->a0,a->a1);return 0;}
static AAsset * (*target_4)(AAssetManager *,const char *,int);
static int64_t invoke_4(JNIEnv *env,KljBionicNdkPacket *packet){Args_4 *a=(void*)(uintptr_t)packet->arguments;a->result=target_4(a->a0,a->a1,a->a2);return 0;}
static AAssetDir * (*target_5)(AAssetManager *,const char *);
static int64_t invoke_5(JNIEnv *env,KljBionicNdkPacket *packet){Args_5 *a=(void*)(uintptr_t)packet->arguments;a->result=target_5(a->a0,a->a1);return 0;}
static void (*target_6)(AAsset *);
static int64_t invoke_6(JNIEnv *env,KljBionicNdkPacket *packet){Args_6 *a=(void*)(uintptr_t)packet->arguments;target_6(a->a0);return 0;}
static const void * (*target_7)(AAsset *);
static int64_t invoke_7(JNIEnv *env,KljBionicNdkPacket *packet){Args_7 *a=(void*)(uintptr_t)packet->arguments;a->result=target_7(a->a0);return 0;}
static off_t (*target_8)(AAsset *);
static int64_t invoke_8(JNIEnv *env,KljBionicNdkPacket *packet){Args_8 *a=(void*)(uintptr_t)packet->arguments;a->result=target_8(a->a0);return 0;}
static off64_t (*target_9)(AAsset *);
static int64_t invoke_9(JNIEnv *env,KljBionicNdkPacket *packet){Args_9 *a=(void*)(uintptr_t)packet->arguments;a->result=target_9(a->a0);return 0;}
static off_t (*target_10)(AAsset *);
static int64_t invoke_10(JNIEnv *env,KljBionicNdkPacket *packet){Args_10 *a=(void*)(uintptr_t)packet->arguments;a->result=target_10(a->a0);return 0;}
static off64_t (*target_11)(AAsset *);
static int64_t invoke_11(JNIEnv *env,KljBionicNdkPacket *packet){Args_11 *a=(void*)(uintptr_t)packet->arguments;a->result=target_11(a->a0);return 0;}
static int (*target_12)(AAsset *);
static int64_t invoke_12(JNIEnv *env,KljBionicNdkPacket *packet){Args_12 *a=(void*)(uintptr_t)packet->arguments;a->result=target_12(a->a0);return 0;}
static int (*target_13)(AAsset *,off_t *,off_t *);
static int64_t invoke_13(JNIEnv *env,KljBionicNdkPacket *packet){Args_13 *a=(void*)(uintptr_t)packet->arguments;a->result=target_13(a->a0,a->a1,a->a2);if(a->result>=0){int previous=errno,sent=klj_bionic_ndk_send_descriptor(packet->descriptor_socket,a->result);close(a->result);a->result=-1;if(sent){errno=kl_errno_from_linux(sent);}else{a->result=0;errno=previous;}}return 0;}
static int (*target_14)(AAsset *,off64_t *,off64_t *);
static int64_t invoke_14(JNIEnv *env,KljBionicNdkPacket *packet){Args_14 *a=(void*)(uintptr_t)packet->arguments;a->result=target_14(a->a0,a->a1,a->a2);if(a->result>=0){int previous=errno,sent=klj_bionic_ndk_send_descriptor(packet->descriptor_socket,a->result);close(a->result);a->result=-1;if(sent){errno=kl_errno_from_linux(sent);}else{a->result=0;errno=previous;}}return 0;}
static int (*target_15)(AAsset *,void *,size_t);
static int64_t invoke_15(JNIEnv *env,KljBionicNdkPacket *packet){Args_15 *a=(void*)(uintptr_t)packet->arguments;a->result=target_15(a->a0,a->a1,a->a2);return 0;}
static off_t (*target_16)(AAsset *,off_t,int);
static int64_t invoke_16(JNIEnv *env,KljBionicNdkPacket *packet){Args_16 *a=(void*)(uintptr_t)packet->arguments;a->result=target_16(a->a0,a->a1,a->a2);return 0;}
static off64_t (*target_17)(AAsset *,off64_t,int);
static int64_t invoke_17(JNIEnv *env,KljBionicNdkPacket *packet){Args_17 *a=(void*)(uintptr_t)packet->arguments;a->result=target_17(a->a0,a->a1,a->a2);return 0;}
static void (*target_18)(AConfiguration *,AConfiguration *);
static int64_t invoke_18(JNIEnv *env,KljBionicNdkPacket *packet){Args_18 *a=(void*)(uintptr_t)packet->arguments;target_18(a->a0,a->a1);return 0;}
static void (*target_19)(AConfiguration *);
static int64_t invoke_19(JNIEnv *env,KljBionicNdkPacket *packet){Args_19 *a=(void*)(uintptr_t)packet->arguments;target_19(a->a0);return 0;}
static int32_t (*target_20)(AConfiguration *,AConfiguration *);
static int64_t invoke_20(JNIEnv *env,KljBionicNdkPacket *packet){Args_20 *a=(void*)(uintptr_t)packet->arguments;a->result=target_20(a->a0,a->a1);return 0;}
static void (*target_21)(AConfiguration *,AAssetManager *);
static int64_t invoke_21(JNIEnv *env,KljBionicNdkPacket *packet){Args_21 *a=(void*)(uintptr_t)packet->arguments;target_21(a->a0,a->a1);return 0;}
static void (*target_22)(AConfiguration *,char *);
static int64_t invoke_22(JNIEnv *env,KljBionicNdkPacket *packet){Args_22 *a=(void*)(uintptr_t)packet->arguments;target_22(a->a0,a->a1);return 0;}
static int32_t (*target_23)(AConfiguration *);
static int64_t invoke_23(JNIEnv *env,KljBionicNdkPacket *packet){Args_23 *a=(void*)(uintptr_t)packet->arguments;a->result=target_23(a->a0);return 0;}
static int32_t (*target_24)(AConfiguration *);
static int64_t invoke_24(JNIEnv *env,KljBionicNdkPacket *packet){Args_24 *a=(void*)(uintptr_t)packet->arguments;a->result=target_24(a->a0);return 0;}
static int32_t (*target_25)(AConfiguration *);
static int64_t invoke_25(JNIEnv *env,KljBionicNdkPacket *packet){Args_25 *a=(void*)(uintptr_t)packet->arguments;a->result=target_25(a->a0);return 0;}
static int32_t (*target_26)(AConfiguration *);
static int64_t invoke_26(JNIEnv *env,KljBionicNdkPacket *packet){Args_26 *a=(void*)(uintptr_t)packet->arguments;a->result=target_26(a->a0);return 0;}
static void (*target_27)(AConfiguration *,char *);
static int64_t invoke_27(JNIEnv *env,KljBionicNdkPacket *packet){Args_27 *a=(void*)(uintptr_t)packet->arguments;target_27(a->a0,a->a1);return 0;}
static int32_t (*target_28)(AConfiguration *);
static int64_t invoke_28(JNIEnv *env,KljBionicNdkPacket *packet){Args_28 *a=(void*)(uintptr_t)packet->arguments;a->result=target_28(a->a0);return 0;}
static int32_t (*target_29)(AConfiguration *);
static int64_t invoke_29(JNIEnv *env,KljBionicNdkPacket *packet){Args_29 *a=(void*)(uintptr_t)packet->arguments;a->result=target_29(a->a0);return 0;}
static int32_t (*target_30)(AConfiguration *);
static int64_t invoke_30(JNIEnv *env,KljBionicNdkPacket *packet){Args_30 *a=(void*)(uintptr_t)packet->arguments;a->result=target_30(a->a0);return 0;}
static int32_t (*target_31)(AConfiguration *);
static int64_t invoke_31(JNIEnv *env,KljBionicNdkPacket *packet){Args_31 *a=(void*)(uintptr_t)packet->arguments;a->result=target_31(a->a0);return 0;}
static int32_t (*target_32)(AConfiguration *);
static int64_t invoke_32(JNIEnv *env,KljBionicNdkPacket *packet){Args_32 *a=(void*)(uintptr_t)packet->arguments;a->result=target_32(a->a0);return 0;}
static int32_t (*target_33)(AConfiguration *);
static int64_t invoke_33(JNIEnv *env,KljBionicNdkPacket *packet){Args_33 *a=(void*)(uintptr_t)packet->arguments;a->result=target_33(a->a0);return 0;}
static int32_t (*target_34)(AConfiguration *);
static int64_t invoke_34(JNIEnv *env,KljBionicNdkPacket *packet){Args_34 *a=(void*)(uintptr_t)packet->arguments;a->result=target_34(a->a0);return 0;}
static int32_t (*target_35)(AConfiguration *);
static int64_t invoke_35(JNIEnv *env,KljBionicNdkPacket *packet){Args_35 *a=(void*)(uintptr_t)packet->arguments;a->result=target_35(a->a0);return 0;}
static int32_t (*target_36)(AConfiguration *);
static int64_t invoke_36(JNIEnv *env,KljBionicNdkPacket *packet){Args_36 *a=(void*)(uintptr_t)packet->arguments;a->result=target_36(a->a0);return 0;}
static int32_t (*target_37)(AConfiguration *);
static int64_t invoke_37(JNIEnv *env,KljBionicNdkPacket *packet){Args_37 *a=(void*)(uintptr_t)packet->arguments;a->result=target_37(a->a0);return 0;}
static int32_t (*target_38)(AConfiguration *);
static int64_t invoke_38(JNIEnv *env,KljBionicNdkPacket *packet){Args_38 *a=(void*)(uintptr_t)packet->arguments;a->result=target_38(a->a0);return 0;}
static int32_t (*target_39)(AConfiguration *);
static int64_t invoke_39(JNIEnv *env,KljBionicNdkPacket *packet){Args_39 *a=(void*)(uintptr_t)packet->arguments;a->result=target_39(a->a0);return 0;}
static int32_t (*target_40)(AConfiguration *);
static int64_t invoke_40(JNIEnv *env,KljBionicNdkPacket *packet){Args_40 *a=(void*)(uintptr_t)packet->arguments;a->result=target_40(a->a0);return 0;}
static int32_t (*target_41)(AConfiguration *);
static int64_t invoke_41(JNIEnv *env,KljBionicNdkPacket *packet){Args_41 *a=(void*)(uintptr_t)packet->arguments;a->result=target_41(a->a0);return 0;}
static int32_t (*target_42)(AConfiguration *);
static int64_t invoke_42(JNIEnv *env,KljBionicNdkPacket *packet){Args_42 *a=(void*)(uintptr_t)packet->arguments;a->result=target_42(a->a0);return 0;}
static int32_t (*target_43)(AConfiguration *);
static int64_t invoke_43(JNIEnv *env,KljBionicNdkPacket *packet){Args_43 *a=(void*)(uintptr_t)packet->arguments;a->result=target_43(a->a0);return 0;}
static int32_t (*target_44)(AConfiguration *,AConfiguration *,AConfiguration *);
static int64_t invoke_44(JNIEnv *env,KljBionicNdkPacket *packet){Args_44 *a=(void*)(uintptr_t)packet->arguments;a->result=target_44(a->a0,a->a1,a->a2);return 0;}
static int32_t (*target_45)(AConfiguration *,AConfiguration *);
static int64_t invoke_45(JNIEnv *env,KljBionicNdkPacket *packet){Args_45 *a=(void*)(uintptr_t)packet->arguments;a->result=target_45(a->a0,a->a1);return 0;}
static AConfiguration * (*target_46)(void);
static int64_t invoke_46(JNIEnv *env,KljBionicNdkPacket *packet){Args_46 *a=(void*)(uintptr_t)packet->arguments;a->result=target_46();return 0;}
static void (*target_47)(AConfiguration *,const char *);
static int64_t invoke_47(JNIEnv *env,KljBionicNdkPacket *packet){Args_47 *a=(void*)(uintptr_t)packet->arguments;target_47(a->a0,a->a1);return 0;}
static void (*target_48)(AConfiguration *,int32_t);
static int64_t invoke_48(JNIEnv *env,KljBionicNdkPacket *packet){Args_48 *a=(void*)(uintptr_t)packet->arguments;target_48(a->a0,a->a1);return 0;}
static void (*target_49)(AConfiguration *,int32_t);
static int64_t invoke_49(JNIEnv *env,KljBionicNdkPacket *packet){Args_49 *a=(void*)(uintptr_t)packet->arguments;target_49(a->a0,a->a1);return 0;}
static void (*target_50)(AConfiguration *,int32_t);
static int64_t invoke_50(JNIEnv *env,KljBionicNdkPacket *packet){Args_50 *a=(void*)(uintptr_t)packet->arguments;target_50(a->a0,a->a1);return 0;}
static void (*target_51)(AConfiguration *,int32_t);
static int64_t invoke_51(JNIEnv *env,KljBionicNdkPacket *packet){Args_51 *a=(void*)(uintptr_t)packet->arguments;target_51(a->a0,a->a1);return 0;}
static void (*target_52)(AConfiguration *,const char *);
static int64_t invoke_52(JNIEnv *env,KljBionicNdkPacket *packet){Args_52 *a=(void*)(uintptr_t)packet->arguments;target_52(a->a0,a->a1);return 0;}
static void (*target_53)(AConfiguration *,int32_t);
static int64_t invoke_53(JNIEnv *env,KljBionicNdkPacket *packet){Args_53 *a=(void*)(uintptr_t)packet->arguments;target_53(a->a0,a->a1);return 0;}
static void (*target_54)(AConfiguration *,int32_t);
static int64_t invoke_54(JNIEnv *env,KljBionicNdkPacket *packet){Args_54 *a=(void*)(uintptr_t)packet->arguments;target_54(a->a0,a->a1);return 0;}
static void (*target_55)(AConfiguration *,int32_t);
static int64_t invoke_55(JNIEnv *env,KljBionicNdkPacket *packet){Args_55 *a=(void*)(uintptr_t)packet->arguments;target_55(a->a0,a->a1);return 0;}
static void (*target_56)(AConfiguration *,int32_t);
static int64_t invoke_56(JNIEnv *env,KljBionicNdkPacket *packet){Args_56 *a=(void*)(uintptr_t)packet->arguments;target_56(a->a0,a->a1);return 0;}
static void (*target_57)(AConfiguration *,int32_t);
static int64_t invoke_57(JNIEnv *env,KljBionicNdkPacket *packet){Args_57 *a=(void*)(uintptr_t)packet->arguments;target_57(a->a0,a->a1);return 0;}
static void (*target_58)(AConfiguration *,int32_t);
static int64_t invoke_58(JNIEnv *env,KljBionicNdkPacket *packet){Args_58 *a=(void*)(uintptr_t)packet->arguments;target_58(a->a0,a->a1);return 0;}
static void (*target_59)(AConfiguration *,int32_t);
static int64_t invoke_59(JNIEnv *env,KljBionicNdkPacket *packet){Args_59 *a=(void*)(uintptr_t)packet->arguments;target_59(a->a0,a->a1);return 0;}
static void (*target_60)(AConfiguration *,int32_t);
static int64_t invoke_60(JNIEnv *env,KljBionicNdkPacket *packet){Args_60 *a=(void*)(uintptr_t)packet->arguments;target_60(a->a0,a->a1);return 0;}
static void (*target_61)(AConfiguration *,int32_t);
static int64_t invoke_61(JNIEnv *env,KljBionicNdkPacket *packet){Args_61 *a=(void*)(uintptr_t)packet->arguments;target_61(a->a0,a->a1);return 0;}
static void (*target_62)(AConfiguration *,int32_t);
static int64_t invoke_62(JNIEnv *env,KljBionicNdkPacket *packet){Args_62 *a=(void*)(uintptr_t)packet->arguments;target_62(a->a0,a->a1);return 0;}
static void (*target_63)(AConfiguration *,int32_t);
static int64_t invoke_63(JNIEnv *env,KljBionicNdkPacket *packet){Args_63 *a=(void*)(uintptr_t)packet->arguments;target_63(a->a0,a->a1);return 0;}
static void (*target_64)(AConfiguration *,int32_t);
static int64_t invoke_64(JNIEnv *env,KljBionicNdkPacket *packet){Args_64 *a=(void*)(uintptr_t)packet->arguments;target_64(a->a0,a->a1);return 0;}
static void (*target_65)(AConfiguration *,int32_t);
static int64_t invoke_65(JNIEnv *env,KljBionicNdkPacket *packet){Args_65 *a=(void*)(uintptr_t)packet->arguments;target_65(a->a0,a->a1);return 0;}
static void (*target_66)(AConfiguration *,int32_t);
static int64_t invoke_66(JNIEnv *env,KljBionicNdkPacket *packet){Args_66 *a=(void*)(uintptr_t)packet->arguments;target_66(a->a0,a->a1);return 0;}
static void (*target_67)(AConfiguration *,int32_t);
static int64_t invoke_67(JNIEnv *env,KljBionicNdkPacket *packet){Args_67 *a=(void*)(uintptr_t)packet->arguments;target_67(a->a0,a->a1);return 0;}
static void (*target_68)(AConfiguration *,int32_t);
static int64_t invoke_68(JNIEnv *env,KljBionicNdkPacket *packet){Args_68 *a=(void*)(uintptr_t)packet->arguments;target_68(a->a0,a->a1);return 0;}
static AImageDecoderFrameInfo * (*target_69)(void);
static int64_t invoke_69(JNIEnv *env,KljBionicNdkPacket *packet){Args_69 *a=(void*)(uintptr_t)packet->arguments;a->result=target_69();return 0;}
static void (*target_70)(AImageDecoderFrameInfo *);
static int64_t invoke_70(JNIEnv *env,KljBionicNdkPacket *packet){Args_70 *a=(void*)(uintptr_t)packet->arguments;target_70(a->a0);return 0;}
static int32_t (*target_71)(const AImageDecoderFrameInfo *);
static int64_t invoke_71(JNIEnv *env,KljBionicNdkPacket *packet){Args_71 *a=(void*)(uintptr_t)packet->arguments;a->result=target_71(a->a0);return 0;}
static int32_t (*target_72)(const AImageDecoderFrameInfo *);
static int64_t invoke_72(JNIEnv *env,KljBionicNdkPacket *packet){Args_72 *a=(void*)(uintptr_t)packet->arguments;a->result=target_72(a->a0);return 0;}
static int64_t (*target_73)(const AImageDecoderFrameInfo *);
static int64_t invoke_73(JNIEnv *env,KljBionicNdkPacket *packet){Args_73 *a=(void*)(uintptr_t)packet->arguments;a->result=target_73(a->a0);return 0;}
static ARect (*target_74)(const AImageDecoderFrameInfo *);
static int64_t invoke_74(JNIEnv *env,KljBionicNdkPacket *packet){Args_74 *a=(void*)(uintptr_t)packet->arguments;a->result=target_74(a->a0);return 0;}
static _Bool (*target_75)(const AImageDecoderFrameInfo *);
static int64_t invoke_75(JNIEnv *env,KljBionicNdkPacket *packet){Args_75 *a=(void*)(uintptr_t)packet->arguments;a->result=target_75(a->a0);return 0;}
static int (*target_76)(const AImageDecoderHeaderInfo *);
static int64_t invoke_76(JNIEnv *env,KljBionicNdkPacket *packet){Args_76 *a=(void*)(uintptr_t)packet->arguments;a->result=target_76(a->a0);return 0;}
static int32_t (*target_77)(const AImageDecoderHeaderInfo *);
static int64_t invoke_77(JNIEnv *env,KljBionicNdkPacket *packet){Args_77 *a=(void*)(uintptr_t)packet->arguments;a->result=target_77(a->a0);return 0;}
static int32_t (*target_78)(const AImageDecoderHeaderInfo *);
static int64_t invoke_78(JNIEnv *env,KljBionicNdkPacket *packet){Args_78 *a=(void*)(uintptr_t)packet->arguments;a->result=target_78(a->a0);return 0;}
static int32_t (*target_79)(const AImageDecoderHeaderInfo *);
static int64_t invoke_79(JNIEnv *env,KljBionicNdkPacket *packet){Args_79 *a=(void*)(uintptr_t)packet->arguments;a->result=target_79(a->a0);return 0;}
static const char * (*target_80)(const AImageDecoderHeaderInfo *);
static int64_t invoke_80(JNIEnv *env,KljBionicNdkPacket *packet){Args_80 *a=(void*)(uintptr_t)packet->arguments;a->result=target_80(a->a0);return 0;}
static int32_t (*target_81)(const AImageDecoderHeaderInfo *);
static int64_t invoke_81(JNIEnv *env,KljBionicNdkPacket *packet){Args_81 *a=(void*)(uintptr_t)packet->arguments;a->result=target_81(a->a0);return 0;}
static int (*target_82)(AImageDecoder *);
static int64_t invoke_82(JNIEnv *env,KljBionicNdkPacket *packet){Args_82 *a=(void*)(uintptr_t)packet->arguments;a->result=target_82(a->a0);return 0;}
static int (*target_83)(const AImageDecoder *,int,int32_t *,int32_t *);
static int64_t invoke_83(JNIEnv *env,KljBionicNdkPacket *packet){Args_83 *a=(void*)(uintptr_t)packet->arguments;a->result=target_83(a->a0,a->a1,a->a2,a->a3);return 0;}
static int (*target_84)(struct AAsset *,AImageDecoder * _Nullable *);
static int64_t invoke_84(JNIEnv *env,KljBionicNdkPacket *packet){Args_84 *a=(void*)(uintptr_t)packet->arguments;a->result=target_84(a->a0,a->a1);return 0;}
static int (*target_85)(const void *,size_t,AImageDecoder * _Nullable *);
static int64_t invoke_85(JNIEnv *env,KljBionicNdkPacket *packet){Args_85 *a=(void*)(uintptr_t)packet->arguments;a->result=target_85(a->a0,a->a1,a->a2);return 0;}
static int (*target_86)(int,AImageDecoder * _Nullable *);
static int64_t invoke_86(JNIEnv *env,KljBionicNdkPacket *packet){Args_86 *a=(void*)(uintptr_t)packet->arguments;if(a->a0<=0||!a->a1){a->result=ANDROID_IMAGE_DECODER_BAD_PARAMETER;return 0;}int native_fd=-1;void *lease=NULL;int64_t acquired=owner.read_lease?owner.read_lease(owner.context,a->a0,&native_fd,&lease):-38;if(acquired){errno=kl_errno_from_linux((int)-acquired);a->result=ANDROID_IMAGE_DECODER_BAD_PARAMETER;return 0;}a->result=target_86(native_fd,a->a1);int saved=errno;owner.release_lease(owner.context,lease);errno=saved;return 0;}
static int (*target_87)(AImageDecoder *,void *,size_t,size_t);
static int64_t invoke_87(JNIEnv *env,KljBionicNdkPacket *packet){Args_87 *a=(void*)(uintptr_t)packet->arguments;a->result=target_87(a->a0,a->a1,a->a2,a->a3);return 0;}
static void (*target_88)(AImageDecoder *);
static int64_t invoke_88(JNIEnv *env,KljBionicNdkPacket *packet){Args_88 *a=(void*)(uintptr_t)packet->arguments;target_88(a->a0);return 0;}
static int (*target_89)(AImageDecoder *,AImageDecoderFrameInfo *);
static int64_t invoke_89(JNIEnv *env,KljBionicNdkPacket *packet){Args_89 *a=(void*)(uintptr_t)packet->arguments;a->result=target_89(a->a0,a->a1);return 0;}
static const AImageDecoderHeaderInfo * (*target_90)(const AImageDecoder *);
static int64_t invoke_90(JNIEnv *env,KljBionicNdkPacket *packet){Args_90 *a=(void*)(uintptr_t)packet->arguments;a->result=target_90(a->a0);return 0;}
static size_t (*target_91)(AImageDecoder *);
static int64_t invoke_91(JNIEnv *env,KljBionicNdkPacket *packet){Args_91 *a=(void*)(uintptr_t)packet->arguments;a->result=target_91(a->a0);return 0;}
static int32_t (*target_92)(AImageDecoder *);
static int64_t invoke_92(JNIEnv *env,KljBionicNdkPacket *packet){Args_92 *a=(void*)(uintptr_t)packet->arguments;a->result=target_92(a->a0);return 0;}
static _Bool (*target_93)(AImageDecoder *);
static int64_t invoke_93(JNIEnv *env,KljBionicNdkPacket *packet){Args_93 *a=(void*)(uintptr_t)packet->arguments;a->result=target_93(a->a0);return 0;}
static const char * (*target_94)(int);
static int64_t invoke_94(JNIEnv *env,KljBionicNdkPacket *packet){Args_94 *a=(void*)(uintptr_t)packet->arguments;a->result=target_94(a->a0);return 0;}
static int (*target_95)(AImageDecoder *);
static int64_t invoke_95(JNIEnv *env,KljBionicNdkPacket *packet){Args_95 *a=(void*)(uintptr_t)packet->arguments;a->result=target_95(a->a0);return 0;}
static int (*target_96)(AImageDecoder *,int32_t);
static int64_t invoke_96(JNIEnv *env,KljBionicNdkPacket *packet){Args_96 *a=(void*)(uintptr_t)packet->arguments;a->result=target_96(a->a0,a->a1);return 0;}
static int (*target_97)(AImageDecoder *,ARect);
static int64_t invoke_97(JNIEnv *env,KljBionicNdkPacket *packet){Args_97 *a=(void*)(uintptr_t)packet->arguments;a->result=target_97(a->a0,a->a1);return 0;}
static int (*target_98)(AImageDecoder *,int32_t);
static int64_t invoke_98(JNIEnv *env,KljBionicNdkPacket *packet){Args_98 *a=(void*)(uintptr_t)packet->arguments;a->result=target_98(a->a0,a->a1);return 0;}
static void (*target_99)(AImageDecoder *,_Bool);
static int64_t invoke_99(JNIEnv *env,KljBionicNdkPacket *packet){Args_99 *a=(void*)(uintptr_t)packet->arguments;target_99(a->a0,a->a1);return 0;}
static int (*target_100)(AImageDecoder *,int32_t,int32_t);
static int64_t invoke_100(JNIEnv *env,KljBionicNdkPacket *packet){Args_100 *a=(void*)(uintptr_t)packet->arguments;a->result=target_100(a->a0,a->a1,a->a2);return 0;}
static int (*target_101)(AImageDecoder *,_Bool);
static int64_t invoke_101(JNIEnv *env,KljBionicNdkPacket *packet){Args_101 *a=(void*)(uintptr_t)packet->arguments;a->result=target_101(a->a0,a->a1);return 0;}
static void (*target_102)(ANativeActivity *);
static int64_t invoke_102(JNIEnv *env,KljBionicNdkPacket *packet){Args_102 *a=(void*)(uintptr_t)packet->arguments;target_102(a->a0);return 0;}
static void (*target_103)(ANativeActivity *,uint32_t);
static int64_t invoke_103(JNIEnv *env,KljBionicNdkPacket *packet){Args_103 *a=(void*)(uintptr_t)packet->arguments;target_103(a->a0,a->a1);return 0;}
static void (*target_104)(ANativeActivity *,uint32_t,uint32_t);
static int64_t invoke_104(JNIEnv *env,KljBionicNdkPacket *packet){Args_104 *a=(void*)(uintptr_t)packet->arguments;target_104(a->a0,a->a1,a->a2);return 0;}
static void (*target_105)(ANativeActivity *,int32_t);
static int64_t invoke_105(JNIEnv *env,KljBionicNdkPacket *packet){Args_105 *a=(void*)(uintptr_t)packet->arguments;target_105(a->a0,a->a1);return 0;}
static void (*target_106)(ANativeActivity *,uint32_t);
static int64_t invoke_106(JNIEnv *env,KljBionicNdkPacket *packet){Args_106 *a=(void*)(uintptr_t)packet->arguments;target_106(a->a0,a->a1);return 0;}
static const KljBionicNdkTarget targets[]={
{(uintptr_t)invoke_0,sizeof(Args_0)},
{(uintptr_t)invoke_1,sizeof(Args_1)},
{(uintptr_t)invoke_2,sizeof(Args_2)},
{(uintptr_t)invoke_3,sizeof(Args_3)},
{(uintptr_t)invoke_4,sizeof(Args_4)},
{(uintptr_t)invoke_5,sizeof(Args_5)},
{(uintptr_t)invoke_6,sizeof(Args_6)},
{(uintptr_t)invoke_7,sizeof(Args_7)},
{(uintptr_t)invoke_8,sizeof(Args_8)},
{(uintptr_t)invoke_9,sizeof(Args_9)},
{(uintptr_t)invoke_10,sizeof(Args_10)},
{(uintptr_t)invoke_11,sizeof(Args_11)},
{(uintptr_t)invoke_12,sizeof(Args_12)},
{(uintptr_t)invoke_13,sizeof(Args_13)},
{(uintptr_t)invoke_14,sizeof(Args_14)},
{(uintptr_t)invoke_15,sizeof(Args_15)},
{(uintptr_t)invoke_16,sizeof(Args_16)},
{(uintptr_t)invoke_17,sizeof(Args_17)},
{(uintptr_t)invoke_18,sizeof(Args_18)},
{(uintptr_t)invoke_19,sizeof(Args_19)},
{(uintptr_t)invoke_20,sizeof(Args_20)},
{(uintptr_t)invoke_21,sizeof(Args_21)},
{(uintptr_t)invoke_22,sizeof(Args_22)},
{(uintptr_t)invoke_23,sizeof(Args_23)},
{(uintptr_t)invoke_24,sizeof(Args_24)},
{(uintptr_t)invoke_25,sizeof(Args_25)},
{(uintptr_t)invoke_26,sizeof(Args_26)},
{(uintptr_t)invoke_27,sizeof(Args_27)},
{(uintptr_t)invoke_28,sizeof(Args_28)},
{(uintptr_t)invoke_29,sizeof(Args_29)},
{(uintptr_t)invoke_30,sizeof(Args_30)},
{(uintptr_t)invoke_31,sizeof(Args_31)},
{(uintptr_t)invoke_32,sizeof(Args_32)},
{(uintptr_t)invoke_33,sizeof(Args_33)},
{(uintptr_t)invoke_34,sizeof(Args_34)},
{(uintptr_t)invoke_35,sizeof(Args_35)},
{(uintptr_t)invoke_36,sizeof(Args_36)},
{(uintptr_t)invoke_37,sizeof(Args_37)},
{(uintptr_t)invoke_38,sizeof(Args_38)},
{(uintptr_t)invoke_39,sizeof(Args_39)},
{(uintptr_t)invoke_40,sizeof(Args_40)},
{(uintptr_t)invoke_41,sizeof(Args_41)},
{(uintptr_t)invoke_42,sizeof(Args_42)},
{(uintptr_t)invoke_43,sizeof(Args_43)},
{(uintptr_t)invoke_44,sizeof(Args_44)},
{(uintptr_t)invoke_45,sizeof(Args_45)},
{(uintptr_t)invoke_46,sizeof(Args_46)},
{(uintptr_t)invoke_47,sizeof(Args_47)},
{(uintptr_t)invoke_48,sizeof(Args_48)},
{(uintptr_t)invoke_49,sizeof(Args_49)},
{(uintptr_t)invoke_50,sizeof(Args_50)},
{(uintptr_t)invoke_51,sizeof(Args_51)},
{(uintptr_t)invoke_52,sizeof(Args_52)},
{(uintptr_t)invoke_53,sizeof(Args_53)},
{(uintptr_t)invoke_54,sizeof(Args_54)},
{(uintptr_t)invoke_55,sizeof(Args_55)},
{(uintptr_t)invoke_56,sizeof(Args_56)},
{(uintptr_t)invoke_57,sizeof(Args_57)},
{(uintptr_t)invoke_58,sizeof(Args_58)},
{(uintptr_t)invoke_59,sizeof(Args_59)},
{(uintptr_t)invoke_60,sizeof(Args_60)},
{(uintptr_t)invoke_61,sizeof(Args_61)},
{(uintptr_t)invoke_62,sizeof(Args_62)},
{(uintptr_t)invoke_63,sizeof(Args_63)},
{(uintptr_t)invoke_64,sizeof(Args_64)},
{(uintptr_t)invoke_65,sizeof(Args_65)},
{(uintptr_t)invoke_66,sizeof(Args_66)},
{(uintptr_t)invoke_67,sizeof(Args_67)},
{(uintptr_t)invoke_68,sizeof(Args_68)},
{(uintptr_t)invoke_69,sizeof(Args_69)},
{(uintptr_t)invoke_70,sizeof(Args_70)},
{(uintptr_t)invoke_71,sizeof(Args_71)},
{(uintptr_t)invoke_72,sizeof(Args_72)},
{(uintptr_t)invoke_73,sizeof(Args_73)},
{(uintptr_t)invoke_74,sizeof(Args_74)},
{(uintptr_t)invoke_75,sizeof(Args_75)},
{(uintptr_t)invoke_76,sizeof(Args_76)},
{(uintptr_t)invoke_77,sizeof(Args_77)},
{(uintptr_t)invoke_78,sizeof(Args_78)},
{(uintptr_t)invoke_79,sizeof(Args_79)},
{(uintptr_t)invoke_80,sizeof(Args_80)},
{(uintptr_t)invoke_81,sizeof(Args_81)},
{(uintptr_t)invoke_82,sizeof(Args_82)},
{(uintptr_t)invoke_83,sizeof(Args_83)},
{(uintptr_t)invoke_84,sizeof(Args_84)},
{(uintptr_t)invoke_85,sizeof(Args_85)},
{(uintptr_t)invoke_86,sizeof(Args_86)},
{(uintptr_t)invoke_87,sizeof(Args_87)},
{(uintptr_t)invoke_88,sizeof(Args_88)},
{(uintptr_t)invoke_89,sizeof(Args_89)},
{(uintptr_t)invoke_90,sizeof(Args_90)},
{(uintptr_t)invoke_91,sizeof(Args_91)},
{(uintptr_t)invoke_92,sizeof(Args_92)},
{(uintptr_t)invoke_93,sizeof(Args_93)},
{(uintptr_t)invoke_94,sizeof(Args_94)},
{(uintptr_t)invoke_95,sizeof(Args_95)},
{(uintptr_t)invoke_96,sizeof(Args_96)},
{(uintptr_t)invoke_97,sizeof(Args_97)},
{(uintptr_t)invoke_98,sizeof(Args_98)},
{(uintptr_t)invoke_99,sizeof(Args_99)},
{(uintptr_t)invoke_100,sizeof(Args_100)},
{(uintptr_t)invoke_101,sizeof(Args_101)},
{(uintptr_t)invoke_102,sizeof(Args_102)},
{(uintptr_t)invoke_103,sizeof(Args_103)},
{(uintptr_t)invoke_104,sizeof(Args_104)},
{(uintptr_t)invoke_105,sizeof(Args_105)},
{(uintptr_t)invoke_106,sizeof(Args_106)},
};
static const KljBionicNdkBindings bindings={sizeof(bindings),KLJ_BIONIC_NDK_ABI,sizeof(targets)/sizeof(*targets),0,targets};
const KljBionicNdkBindings *klj_bionic_ndk_bindings(void){return ready?&bindings:NULL;}
int klj_bionic_ndk_prepare(const KljBionicNdkOwner *input){if(!input||!input->resolve||!input->environment||!!input->read_lease!=!!input->release_lease)return -22;pthread_mutex_lock(&lock);if(ready){int equal=owner.resolve==input->resolve&&owner.environment==input->environment&&owner.context==input->context&&owner.read_lease==input->read_lease&&owner.release_lease==input->release_lease;pthread_mutex_unlock(&lock);return equal?0:-16;}
void *p0=input->resolve("AAssetDir_close");if(!p0){pthread_mutex_unlock(&lock);return -2;}
void *p1=input->resolve("AAssetDir_getNextFileName");if(!p1){pthread_mutex_unlock(&lock);return -2;}
void *p2=input->resolve("AAssetDir_rewind");if(!p2){pthread_mutex_unlock(&lock);return -2;}
void *p3=input->resolve("AAssetManager_fromJava");if(!p3){pthread_mutex_unlock(&lock);return -2;}
void *p4=input->resolve("AAssetManager_open");if(!p4){pthread_mutex_unlock(&lock);return -2;}
void *p5=input->resolve("AAssetManager_openDir");if(!p5){pthread_mutex_unlock(&lock);return -2;}
void *p6=input->resolve("AAsset_close");if(!p6){pthread_mutex_unlock(&lock);return -2;}
void *p7=input->resolve("AAsset_getBuffer");if(!p7){pthread_mutex_unlock(&lock);return -2;}
void *p8=input->resolve("AAsset_getLength");if(!p8){pthread_mutex_unlock(&lock);return -2;}
void *p9=input->resolve("AAsset_getLength64");if(!p9){pthread_mutex_unlock(&lock);return -2;}
void *p10=input->resolve("AAsset_getRemainingLength");if(!p10){pthread_mutex_unlock(&lock);return -2;}
void *p11=input->resolve("AAsset_getRemainingLength64");if(!p11){pthread_mutex_unlock(&lock);return -2;}
void *p12=input->resolve("AAsset_isAllocated");if(!p12){pthread_mutex_unlock(&lock);return -2;}
void *p13=input->resolve("AAsset_openFileDescriptor");if(!p13){pthread_mutex_unlock(&lock);return -2;}
void *p14=input->resolve("AAsset_openFileDescriptor64");if(!p14){pthread_mutex_unlock(&lock);return -2;}
void *p15=input->resolve("AAsset_read");if(!p15){pthread_mutex_unlock(&lock);return -2;}
void *p16=input->resolve("AAsset_seek");if(!p16){pthread_mutex_unlock(&lock);return -2;}
void *p17=input->resolve("AAsset_seek64");if(!p17){pthread_mutex_unlock(&lock);return -2;}
void *p18=input->resolve("AConfiguration_copy");if(!p18){pthread_mutex_unlock(&lock);return -2;}
void *p19=input->resolve("AConfiguration_delete");if(!p19){pthread_mutex_unlock(&lock);return -2;}
void *p20=input->resolve("AConfiguration_diff");if(!p20){pthread_mutex_unlock(&lock);return -2;}
void *p21=input->resolve("AConfiguration_fromAssetManager");if(!p21){pthread_mutex_unlock(&lock);return -2;}
void *p22=input->resolve("AConfiguration_getCountry");if(!p22){pthread_mutex_unlock(&lock);return -2;}
void *p23=input->resolve("AConfiguration_getDensity");if(!p23){pthread_mutex_unlock(&lock);return -2;}
void *p24=input->resolve("AConfiguration_getGrammaticalGender");if(!p24){pthread_mutex_unlock(&lock);return -2;}
void *p25=input->resolve("AConfiguration_getKeyboard");if(!p25){pthread_mutex_unlock(&lock);return -2;}
void *p26=input->resolve("AConfiguration_getKeysHidden");if(!p26){pthread_mutex_unlock(&lock);return -2;}
void *p27=input->resolve("AConfiguration_getLanguage");if(!p27){pthread_mutex_unlock(&lock);return -2;}
void *p28=input->resolve("AConfiguration_getLayoutDirection");if(!p28){pthread_mutex_unlock(&lock);return -2;}
void *p29=input->resolve("AConfiguration_getMcc");if(!p29){pthread_mutex_unlock(&lock);return -2;}
void *p30=input->resolve("AConfiguration_getMnc");if(!p30){pthread_mutex_unlock(&lock);return -2;}
void *p31=input->resolve("AConfiguration_getNavHidden");if(!p31){pthread_mutex_unlock(&lock);return -2;}
void *p32=input->resolve("AConfiguration_getNavigation");if(!p32){pthread_mutex_unlock(&lock);return -2;}
void *p33=input->resolve("AConfiguration_getOrientation");if(!p33){pthread_mutex_unlock(&lock);return -2;}
void *p34=input->resolve("AConfiguration_getScreenHeightDp");if(!p34){pthread_mutex_unlock(&lock);return -2;}
void *p35=input->resolve("AConfiguration_getScreenLong");if(!p35){pthread_mutex_unlock(&lock);return -2;}
void *p36=input->resolve("AConfiguration_getScreenRound");if(!p36){pthread_mutex_unlock(&lock);return -2;}
void *p37=input->resolve("AConfiguration_getScreenSize");if(!p37){pthread_mutex_unlock(&lock);return -2;}
void *p38=input->resolve("AConfiguration_getScreenWidthDp");if(!p38){pthread_mutex_unlock(&lock);return -2;}
void *p39=input->resolve("AConfiguration_getSdkVersion");if(!p39){pthread_mutex_unlock(&lock);return -2;}
void *p40=input->resolve("AConfiguration_getSmallestScreenWidthDp");if(!p40){pthread_mutex_unlock(&lock);return -2;}
void *p41=input->resolve("AConfiguration_getTouchscreen");if(!p41){pthread_mutex_unlock(&lock);return -2;}
void *p42=input->resolve("AConfiguration_getUiModeNight");if(!p42){pthread_mutex_unlock(&lock);return -2;}
void *p43=input->resolve("AConfiguration_getUiModeType");if(!p43){pthread_mutex_unlock(&lock);return -2;}
void *p44=input->resolve("AConfiguration_isBetterThan");if(!p44){pthread_mutex_unlock(&lock);return -2;}
void *p45=input->resolve("AConfiguration_match");if(!p45){pthread_mutex_unlock(&lock);return -2;}
void *p46=input->resolve("AConfiguration_new");if(!p46){pthread_mutex_unlock(&lock);return -2;}
void *p47=input->resolve("AConfiguration_setCountry");if(!p47){pthread_mutex_unlock(&lock);return -2;}
void *p48=input->resolve("AConfiguration_setDensity");if(!p48){pthread_mutex_unlock(&lock);return -2;}
void *p49=input->resolve("AConfiguration_setGrammaticalGender");if(!p49){pthread_mutex_unlock(&lock);return -2;}
void *p50=input->resolve("AConfiguration_setKeyboard");if(!p50){pthread_mutex_unlock(&lock);return -2;}
void *p51=input->resolve("AConfiguration_setKeysHidden");if(!p51){pthread_mutex_unlock(&lock);return -2;}
void *p52=input->resolve("AConfiguration_setLanguage");if(!p52){pthread_mutex_unlock(&lock);return -2;}
void *p53=input->resolve("AConfiguration_setLayoutDirection");if(!p53){pthread_mutex_unlock(&lock);return -2;}
void *p54=input->resolve("AConfiguration_setMcc");if(!p54){pthread_mutex_unlock(&lock);return -2;}
void *p55=input->resolve("AConfiguration_setMnc");if(!p55){pthread_mutex_unlock(&lock);return -2;}
void *p56=input->resolve("AConfiguration_setNavHidden");if(!p56){pthread_mutex_unlock(&lock);return -2;}
void *p57=input->resolve("AConfiguration_setNavigation");if(!p57){pthread_mutex_unlock(&lock);return -2;}
void *p58=input->resolve("AConfiguration_setOrientation");if(!p58){pthread_mutex_unlock(&lock);return -2;}
void *p59=input->resolve("AConfiguration_setScreenHeightDp");if(!p59){pthread_mutex_unlock(&lock);return -2;}
void *p60=input->resolve("AConfiguration_setScreenLong");if(!p60){pthread_mutex_unlock(&lock);return -2;}
void *p61=input->resolve("AConfiguration_setScreenRound");if(!p61){pthread_mutex_unlock(&lock);return -2;}
void *p62=input->resolve("AConfiguration_setScreenSize");if(!p62){pthread_mutex_unlock(&lock);return -2;}
void *p63=input->resolve("AConfiguration_setScreenWidthDp");if(!p63){pthread_mutex_unlock(&lock);return -2;}
void *p64=input->resolve("AConfiguration_setSdkVersion");if(!p64){pthread_mutex_unlock(&lock);return -2;}
void *p65=input->resolve("AConfiguration_setSmallestScreenWidthDp");if(!p65){pthread_mutex_unlock(&lock);return -2;}
void *p66=input->resolve("AConfiguration_setTouchscreen");if(!p66){pthread_mutex_unlock(&lock);return -2;}
void *p67=input->resolve("AConfiguration_setUiModeNight");if(!p67){pthread_mutex_unlock(&lock);return -2;}
void *p68=input->resolve("AConfiguration_setUiModeType");if(!p68){pthread_mutex_unlock(&lock);return -2;}
void *p69=input->resolve("AImageDecoderFrameInfo_create");if(!p69){pthread_mutex_unlock(&lock);return -2;}
void *p70=input->resolve("AImageDecoderFrameInfo_delete");if(!p70){pthread_mutex_unlock(&lock);return -2;}
void *p71=input->resolve("AImageDecoderFrameInfo_getBlendOp");if(!p71){pthread_mutex_unlock(&lock);return -2;}
void *p72=input->resolve("AImageDecoderFrameInfo_getDisposeOp");if(!p72){pthread_mutex_unlock(&lock);return -2;}
void *p73=input->resolve("AImageDecoderFrameInfo_getDuration");if(!p73){pthread_mutex_unlock(&lock);return -2;}
void *p74=input->resolve("AImageDecoderFrameInfo_getFrameRect");if(!p74){pthread_mutex_unlock(&lock);return -2;}
void *p75=input->resolve("AImageDecoderFrameInfo_hasAlphaWithinBounds");if(!p75){pthread_mutex_unlock(&lock);return -2;}
void *p76=input->resolve("AImageDecoderHeaderInfo_getAlphaFlags");if(!p76){pthread_mutex_unlock(&lock);return -2;}
void *p77=input->resolve("AImageDecoderHeaderInfo_getAndroidBitmapFormat");if(!p77){pthread_mutex_unlock(&lock);return -2;}
void *p78=input->resolve("AImageDecoderHeaderInfo_getDataSpace");if(!p78){pthread_mutex_unlock(&lock);return -2;}
void *p79=input->resolve("AImageDecoderHeaderInfo_getHeight");if(!p79){pthread_mutex_unlock(&lock);return -2;}
void *p80=input->resolve("AImageDecoderHeaderInfo_getMimeType");if(!p80){pthread_mutex_unlock(&lock);return -2;}
void *p81=input->resolve("AImageDecoderHeaderInfo_getWidth");if(!p81){pthread_mutex_unlock(&lock);return -2;}
void *p82=input->resolve("AImageDecoder_advanceFrame");if(!p82){pthread_mutex_unlock(&lock);return -2;}
void *p83=input->resolve("AImageDecoder_computeSampledSize");if(!p83){pthread_mutex_unlock(&lock);return -2;}
void *p84=input->resolve("AImageDecoder_createFromAAsset");if(!p84){pthread_mutex_unlock(&lock);return -2;}
void *p85=input->resolve("AImageDecoder_createFromBuffer");if(!p85){pthread_mutex_unlock(&lock);return -2;}
void *p86=input->resolve("AImageDecoder_createFromFd");if(!p86){pthread_mutex_unlock(&lock);return -2;}
void *p87=input->resolve("AImageDecoder_decodeImage");if(!p87){pthread_mutex_unlock(&lock);return -2;}
void *p88=input->resolve("AImageDecoder_delete");if(!p88){pthread_mutex_unlock(&lock);return -2;}
void *p89=input->resolve("AImageDecoder_getFrameInfo");if(!p89){pthread_mutex_unlock(&lock);return -2;}
void *p90=input->resolve("AImageDecoder_getHeaderInfo");if(!p90){pthread_mutex_unlock(&lock);return -2;}
void *p91=input->resolve("AImageDecoder_getMinimumStride");if(!p91){pthread_mutex_unlock(&lock);return -2;}
void *p92=input->resolve("AImageDecoder_getRepeatCount");if(!p92){pthread_mutex_unlock(&lock);return -2;}
void *p93=input->resolve("AImageDecoder_isAnimated");if(!p93){pthread_mutex_unlock(&lock);return -2;}
void *p94=input->resolve("AImageDecoder_resultToString");if(!p94){pthread_mutex_unlock(&lock);return -2;}
void *p95=input->resolve("AImageDecoder_rewind");if(!p95){pthread_mutex_unlock(&lock);return -2;}
void *p96=input->resolve("AImageDecoder_setAndroidBitmapFormat");if(!p96){pthread_mutex_unlock(&lock);return -2;}
void *p97=input->resolve("AImageDecoder_setCrop");if(!p97){pthread_mutex_unlock(&lock);return -2;}
void *p98=input->resolve("AImageDecoder_setDataSpace");if(!p98){pthread_mutex_unlock(&lock);return -2;}
void *p99=input->resolve("AImageDecoder_setInternallyHandleDisposePrevious");if(!p99){pthread_mutex_unlock(&lock);return -2;}
void *p100=input->resolve("AImageDecoder_setTargetSize");if(!p100){pthread_mutex_unlock(&lock);return -2;}
void *p101=input->resolve("AImageDecoder_setUnpremultipliedRequired");if(!p101){pthread_mutex_unlock(&lock);return -2;}
void *p102=input->resolve("ANativeActivity_finish");if(!p102){pthread_mutex_unlock(&lock);return -2;}
void *p103=input->resolve("ANativeActivity_hideSoftInput");if(!p103){pthread_mutex_unlock(&lock);return -2;}
void *p104=input->resolve("ANativeActivity_setWindowFlags");if(!p104){pthread_mutex_unlock(&lock);return -2;}
void *p105=input->resolve("ANativeActivity_setWindowFormat");if(!p105){pthread_mutex_unlock(&lock);return -2;}
void *p106=input->resolve("ANativeActivity_showSoftInput");if(!p106){pthread_mutex_unlock(&lock);return -2;}
memcpy(&target_0,&p0,sizeof(target_0));
memcpy(&target_1,&p1,sizeof(target_1));
memcpy(&target_2,&p2,sizeof(target_2));
memcpy(&target_3,&p3,sizeof(target_3));
memcpy(&target_4,&p4,sizeof(target_4));
memcpy(&target_5,&p5,sizeof(target_5));
memcpy(&target_6,&p6,sizeof(target_6));
memcpy(&target_7,&p7,sizeof(target_7));
memcpy(&target_8,&p8,sizeof(target_8));
memcpy(&target_9,&p9,sizeof(target_9));
memcpy(&target_10,&p10,sizeof(target_10));
memcpy(&target_11,&p11,sizeof(target_11));
memcpy(&target_12,&p12,sizeof(target_12));
memcpy(&target_13,&p13,sizeof(target_13));
memcpy(&target_14,&p14,sizeof(target_14));
memcpy(&target_15,&p15,sizeof(target_15));
memcpy(&target_16,&p16,sizeof(target_16));
memcpy(&target_17,&p17,sizeof(target_17));
memcpy(&target_18,&p18,sizeof(target_18));
memcpy(&target_19,&p19,sizeof(target_19));
memcpy(&target_20,&p20,sizeof(target_20));
memcpy(&target_21,&p21,sizeof(target_21));
memcpy(&target_22,&p22,sizeof(target_22));
memcpy(&target_23,&p23,sizeof(target_23));
memcpy(&target_24,&p24,sizeof(target_24));
memcpy(&target_25,&p25,sizeof(target_25));
memcpy(&target_26,&p26,sizeof(target_26));
memcpy(&target_27,&p27,sizeof(target_27));
memcpy(&target_28,&p28,sizeof(target_28));
memcpy(&target_29,&p29,sizeof(target_29));
memcpy(&target_30,&p30,sizeof(target_30));
memcpy(&target_31,&p31,sizeof(target_31));
memcpy(&target_32,&p32,sizeof(target_32));
memcpy(&target_33,&p33,sizeof(target_33));
memcpy(&target_34,&p34,sizeof(target_34));
memcpy(&target_35,&p35,sizeof(target_35));
memcpy(&target_36,&p36,sizeof(target_36));
memcpy(&target_37,&p37,sizeof(target_37));
memcpy(&target_38,&p38,sizeof(target_38));
memcpy(&target_39,&p39,sizeof(target_39));
memcpy(&target_40,&p40,sizeof(target_40));
memcpy(&target_41,&p41,sizeof(target_41));
memcpy(&target_42,&p42,sizeof(target_42));
memcpy(&target_43,&p43,sizeof(target_43));
memcpy(&target_44,&p44,sizeof(target_44));
memcpy(&target_45,&p45,sizeof(target_45));
memcpy(&target_46,&p46,sizeof(target_46));
memcpy(&target_47,&p47,sizeof(target_47));
memcpy(&target_48,&p48,sizeof(target_48));
memcpy(&target_49,&p49,sizeof(target_49));
memcpy(&target_50,&p50,sizeof(target_50));
memcpy(&target_51,&p51,sizeof(target_51));
memcpy(&target_52,&p52,sizeof(target_52));
memcpy(&target_53,&p53,sizeof(target_53));
memcpy(&target_54,&p54,sizeof(target_54));
memcpy(&target_55,&p55,sizeof(target_55));
memcpy(&target_56,&p56,sizeof(target_56));
memcpy(&target_57,&p57,sizeof(target_57));
memcpy(&target_58,&p58,sizeof(target_58));
memcpy(&target_59,&p59,sizeof(target_59));
memcpy(&target_60,&p60,sizeof(target_60));
memcpy(&target_61,&p61,sizeof(target_61));
memcpy(&target_62,&p62,sizeof(target_62));
memcpy(&target_63,&p63,sizeof(target_63));
memcpy(&target_64,&p64,sizeof(target_64));
memcpy(&target_65,&p65,sizeof(target_65));
memcpy(&target_66,&p66,sizeof(target_66));
memcpy(&target_67,&p67,sizeof(target_67));
memcpy(&target_68,&p68,sizeof(target_68));
memcpy(&target_69,&p69,sizeof(target_69));
memcpy(&target_70,&p70,sizeof(target_70));
memcpy(&target_71,&p71,sizeof(target_71));
memcpy(&target_72,&p72,sizeof(target_72));
memcpy(&target_73,&p73,sizeof(target_73));
memcpy(&target_74,&p74,sizeof(target_74));
memcpy(&target_75,&p75,sizeof(target_75));
memcpy(&target_76,&p76,sizeof(target_76));
memcpy(&target_77,&p77,sizeof(target_77));
memcpy(&target_78,&p78,sizeof(target_78));
memcpy(&target_79,&p79,sizeof(target_79));
memcpy(&target_80,&p80,sizeof(target_80));
memcpy(&target_81,&p81,sizeof(target_81));
memcpy(&target_82,&p82,sizeof(target_82));
memcpy(&target_83,&p83,sizeof(target_83));
memcpy(&target_84,&p84,sizeof(target_84));
memcpy(&target_85,&p85,sizeof(target_85));
memcpy(&target_86,&p86,sizeof(target_86));
memcpy(&target_87,&p87,sizeof(target_87));
memcpy(&target_88,&p88,sizeof(target_88));
memcpy(&target_89,&p89,sizeof(target_89));
memcpy(&target_90,&p90,sizeof(target_90));
memcpy(&target_91,&p91,sizeof(target_91));
memcpy(&target_92,&p92,sizeof(target_92));
memcpy(&target_93,&p93,sizeof(target_93));
memcpy(&target_94,&p94,sizeof(target_94));
memcpy(&target_95,&p95,sizeof(target_95));
memcpy(&target_96,&p96,sizeof(target_96));
memcpy(&target_97,&p97,sizeof(target_97));
memcpy(&target_98,&p98,sizeof(target_98));
memcpy(&target_99,&p99,sizeof(target_99));
memcpy(&target_100,&p100,sizeof(target_100));
memcpy(&target_101,&p101,sizeof(target_101));
memcpy(&target_102,&p102,sizeof(target_102));
memcpy(&target_103,&p103,sizeof(target_103));
memcpy(&target_104,&p104,sizeof(target_104));
memcpy(&target_105,&p105,sizeof(target_105));
memcpy(&target_106,&p106,sizeof(target_106));
owner=*input;ready=1;pthread_mutex_unlock(&lock);return 0;}
int64_t klj_bionic_ndk_dispatch(JNIEnv *env,KljBionicNdkPacket *packet){if(!ready||!packet||packet->size!=sizeof(*packet)||packet->version!=KLJ_BIONIC_NDK_ABI||!packet->arguments)return -22;for(unsigned i=0;i<bindings.count;i++){if(packet->target!=targets[i].target)continue;if(packet->argument_size!=targets[i].argument_size)return -22;int saved=errno;errno=kl_errno_from_linux(packet->error);int64_t status=((int64_t(*)(JNIEnv*,KljBionicNdkPacket*))(uintptr_t)packet->target)(env,packet);packet->error=kl_errno_to_linux(errno);errno=saved;return status;}return -1;}
