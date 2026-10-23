#include "jvm-bionic-jni.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
extern int64_t fixture_host(uint32_t,void*,size_t);
JNIEXPORT jlong JNICALL Java_BionicProviderProbe_roundTrip(JNIEnv *env,jclass type,jint a,jlong b,jfloat c,jdouble d,jint e,jint f,jint g,jint h,jint i,jint j){
 assert(env&&type&&a==11&&b==22&&c==1.5f&&d==2.5&&e==33&&f==44&&g==55&&h==66&&i==77&&j==88);
 int *tls=__errno();errno=219;assert(pthread_self()&&getpid()>0);
 FixtureJavaCallback callback={.env=env,.type=type,.argument={.j=a+b+(jlong)c+(jlong)d+e+f+g+h+i+j}};
 assert(fixture_host(4,&callback,sizeof callback)==0&&__errno()==tls&&errno==219);
 return callback.result;
}
