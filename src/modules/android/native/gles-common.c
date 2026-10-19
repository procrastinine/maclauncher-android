#include "gles-common.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
static void*(*resolver)(const char*);
void klj_next_set_resolver(void*(*value)(const char*)){resolver=value;}
void klj_next_throw(JNIEnv*e,const char*type,const char*message){if((*e)->ExceptionCheck(e))return;jclass c=(*e)->FindClass(e,type);if(c){(*e)->ThrowNew(e,c,message);(*e)->DeleteLocalRef(e,c);}}
void*klj_next_symbol(JNIEnv*e,const char*name){void*p=resolver?resolver(name):NULL;if(!p){char message[160];snprintf(message,sizeof message,"Required GLES function is unavailable: %.96s",name);klj_next_throw(e,"java/lang/UnsatisfiedLinkError",message);}return p;}
static void bad(JNIEnv*e,const char*message){klj_next_throw(e,"java/lang/IllegalArgumentException",message);}
int klj_next_array_range(JNIEnv*e,jarray value,jint offset,jint count){if(!value||offset<0||count<0||offset>(*e)->GetArrayLength(e,value)-count){bad(e,"GLES array range is invalid");return 0;}return 1;}
void*klj_next_buffer(JNIEnv*e,jobject value,int64_t bytes,int output){
    if(!value||bytes<0){bad(e,"GLES buffer or byte count is invalid");return NULL;}
    jclass base=(*e)->GetObjectClass(e,value);if(!base)return NULL;
    jmethodID position=(*e)->GetMethodID(e,base,"position","()I");if((*e)->ExceptionCheck(e)){(*e)->DeleteLocalRef(e,base);return NULL;}
    jmethodID remaining=(*e)->GetMethodID(e,base,"remaining","()I");if((*e)->ExceptionCheck(e)){(*e)->DeleteLocalRef(e,base);return NULL;}
    jmethodID readonly=(*e)->GetMethodID(e,base,"isReadOnly","()Z");if((*e)->ExceptionCheck(e)){(*e)->DeleteLocalRef(e,base);return NULL;}
    jmethodID order=(*e)->GetMethodID(e,base,"order","()Ljava/nio/ByteOrder;");(*e)->DeleteLocalRef(e,base);if((*e)->ExceptionCheck(e))return NULL;
    jint pos=(*e)->CallIntMethod(e,value,position);if((*e)->ExceptionCheck(e))return NULL;
    jint count=(*e)->CallIntMethod(e,value,remaining);if((*e)->ExceptionCheck(e))return NULL;
    if(output){jboolean ro=(*e)->CallBooleanMethod(e,value,readonly);if((*e)->ExceptionCheck(e))return NULL;if(ro){bad(e,"GLES output buffer is read-only");return NULL;}}
    const char*names[]={"ByteBuffer","ShortBuffer","CharBuffer","IntBuffer","FloatBuffer","LongBuffer","DoubleBuffer"};const int sizes[]={1,2,2,4,4,8,8};int element=0;
    for(int i=0;i<7&&!element;i++){char name[64];snprintf(name,sizeof name,"java/nio/%s",names[i]);jclass type=(*e)->FindClass(e,name);if(!type)return NULL;if((*e)->IsInstanceOf(e,value,type))element=sizes[i];(*e)->DeleteLocalRef(e,type);}
    if(element>1){
        jobject actual=(*e)->CallObjectMethod(e,value,order);if((*e)->ExceptionCheck(e))return NULL;
        jclass type=(*e)->FindClass(e,"java/nio/ByteOrder");if(!type){(*e)->DeleteLocalRef(e,actual);return NULL;}
        jmethodID nativeOrder=(*e)->GetStaticMethodID(e,type,"nativeOrder","()Ljava/nio/ByteOrder;");if((*e)->ExceptionCheck(e)){(*e)->DeleteLocalRef(e,type);(*e)->DeleteLocalRef(e,actual);return NULL;}
        jobject expected=(*e)->CallStaticObjectMethod(e,type,nativeOrder);(*e)->DeleteLocalRef(e,type);if((*e)->ExceptionCheck(e)){(*e)->DeleteLocalRef(e,actual);return NULL;}
        int same=(*e)->IsSameObject(e,actual,expected);(*e)->DeleteLocalRef(e,actual);(*e)->DeleteLocalRef(e,expected);if(!same){bad(e,"GLES typed buffers require native byte order");return NULL;}
    }
    void*address=(*e)->GetDirectBufferAddress(e,value);jlong capacity=(*e)->GetDirectBufferCapacity(e,value);
    if(!element||capacity<0||pos<0||count<0||capacity<(int64_t)pos+count||(int64_t)count*element<bytes||(!address&&bytes)){bad(e,"GLES requires a direct buffer with sufficient remaining bytes");return NULL;}
    return address?(unsigned char*)address+(size_t)pos*element:NULL;
}
int klj_next_query_count(JNIEnv*e,unsigned parameter){
    switch(parameter){case 0x0BA2:case 0x0C10:case 0x0C22:case 0x0C23:case 0x8005:return 4;
    case 0x0B70:case 0x0D3A:case 0x846D:case 0x846E:return 2;
    case 0x86A3:case 0x8DF8:case 0x87FF:{
        void(*get)(unsigned,int*)=(void(*)(unsigned,int*))klj_next_symbol(e,"glGetIntegerv");if(!get)return 0;int n=0;get(parameter==0x86A3?0x86A2:parameter==0x8DF8?0x8DF9:0x87FE,&n);if(n<0){bad(e,"GLES query result is invalid");return 0;}return n;}
    default:return 1;}
}
static int64_t multiply(JNIEnv*e,int64_t a,int64_t b){if(a<0||b<0||(a&&b>INT64_MAX/a)){bad(e,"GLES pixel range overflows");return -1;}return a*b;}
static int uniformWidth(unsigned type){
    switch(type){case 0x1404:case 0x1405:case 0x1406:case 0x8B56:case 0x8D66:return 1;
    case 0x8B50:case 0x8B53:case 0x8B57:case 0x8DC6:return 2;
    case 0x8B51:case 0x8B54:case 0x8B58:case 0x8DC7:return 3;
    case 0x8B52:case 0x8B55:case 0x8B59:case 0x8DC8:case 0x8B5A:return 4;
    case 0x8B5B:return 9;case 0x8B5C:return 16;
    case 0x8B65:case 0x8B67:return 6;case 0x8B66:case 0x8B69:return 8;case 0x8B68:case 0x8B6A:return 12;
    default:if((type>=0x8B5D&&type<=0x8B64)||(type>=0x8DC0&&type<=0x8DC5)||(type>=0x8DC9&&type<=0x8DDF)||(type>=0x9108&&type<=0x910D)||(type>=0x900C&&type<=0x900F)||(type>=0x904D&&type<=0x906D)||type==0x92DB)return 1;return 0;}
}
int64_t klj_next_uniform_components(JNIEnv*e,unsigned program,int location){
    void(*query)(unsigned,unsigned,int*)=(void(*)(unsigned,unsigned,int*))klj_next_symbol(e,"glGetProgramiv");
    void(*active)(unsigned,unsigned,int,int*,int*,unsigned*,char*)=(void(*)(unsigned,unsigned,int,int*,int*,unsigned*,char*))klj_next_symbol(e,"glGetActiveUniform");
    int(*locate)(unsigned,const char*)=(int(*)(unsigned,const char*))klj_next_symbol(e,"glGetUniformLocation");if(!query||!active||!locate)return -1;
    int count=0,maxName=0;query(program,0x8B86,&count);query(program,0x8B87,&maxName);
    if(count<0||maxName<0){bad(e,"GLES uniform metadata is invalid");return -1;}
    if(location<0||!count||!maxName){bad(e,"GLES uniform location is unavailable");return -1;}
    char*name=calloc((size_t)maxName+1,1),*element=calloc((size_t)maxName+32,1);if(!name||!element){free(name);free(element);klj_next_throw(e,"java/lang/OutOfMemoryError","GLES uniform metadata allocation failed");return -1;}
    int width=0;
    for(int i=0;i<count&&!width;i++){
        int used=0,size=0;unsigned type=0;active(program,(unsigned)i,maxName,&used,&size,&type,name);
        if(used<0||used>=maxName||size<0){bad(e,"GLES uniform metadata is invalid");break;}name[used]=0;
        int match=locate(program,name)==location;
        char*array=strstr(name,"[0]");
        if(!match&&size>1&&array){
            for(int n=1;n<size;n++){snprintf(element,(size_t)maxName+32,"%.*s[%d]%s",(int)(array-name),name,n,array+3);if(locate(program,element)==location){match=1;break;}}
        }
        if(match){width=uniformWidth(type);if(!width)bad(e,"GLES uniform type is unsupported");}
    }
    free(name);free(element);if((*e)->ExceptionCheck(e))return -1;if(!width){bad(e,"GLES uniform location is unavailable");return -1;}return width;
}
static int64_t add(JNIEnv*e,int64_t a,int64_t b){if(a<0||b<0||a>INT64_MAX-b){bad(e,"GLES pixel range overflows");return -1;}return a+b;}
int64_t klj_next_pixel_bytes(JNIEnv*e,int w,int h,int d,unsigned format,unsigned type,int pack){
    int volume=pack&2;pack&=1;
    if(w<0||h<0||d<0){bad(e,"GLES image dimensions are negative");return -1;}if(!w||!h||!d)return 0;
    int components=0,bytes=0;
    switch(format){case 0x1902:case 0x1903:case 0x1906:case 0x1909:case 0x8D94:components=1;break;case 0x190A:case 0x8227:case 0x8228:components=2;break;case 0x1907:case 0x8D98:components=3;break;case 0x1908:case 0x80E1:case 0x8D99:components=4;break;case 0x84F9:components=1;break;default:bad(e,"Unsupported GLES pixel format");return -1;}
    switch(type){case 0x1400:case 0x1401:bytes=components;break;case 0x1402:case 0x1403:case 0x140B:case 0x8D61:bytes=2*components;break;case 0x1404:case 0x1405:case 0x1406:bytes=4*components;break;case 0x8033:case 0x8034:case 0x8363:bytes=2;break;case 0x8035:case 0x8036:case 0x8368:case 0x8C3B:case 0x8C3E:case 0x84FA:bytes=4;break;case 0x8DAD:bytes=8;break;default:bad(e,"Unsupported GLES pixel type");return -1;}
    void(*get)(unsigned,int*)=(void(*)(unsigned,int*))klj_next_symbol(e,"glGetIntegerv");const unsigned char*(*string)(unsigned)=(const unsigned char*(*)(unsigned))klj_next_symbol(e,"glGetString");if(!get||!string)return -1;
    int alignment=0,rowLength=0,imageHeight=0,skipPixels=0,skipRows=0,skipImages=0;get(pack?0x0D05:0x0CF5,&alignment);
    if(alignment!=1&&alignment!=2&&alignment!=4&&alignment!=8){bad(e,"GLES pixel alignment is invalid");return -1;}
    const char*version=(const char*)string(0x1F02);int major=0;if(version)sscanf(version,"OpenGL ES %d",&major);
    if(major>=3){get(pack?0x0D02:0x0CF2,&rowLength);get(pack?0x0D04:0x0CF4,&skipPixels);get(pack?0x0D03:0x0CF3,&skipRows);if(!pack&&volume){get(0x806E,&imageHeight);get(0x806D,&skipImages);}}
    if(rowLength<0||imageHeight<0||skipPixels<0||skipRows<0||skipImages<0){bad(e,"GLES pixel storage state is invalid");return -1;}
    int64_t row=multiply(e,rowLength?rowLength:w,bytes);if(row<0)return -1;
    int64_t aligned=add(e,row,alignment-1);if(aligned<0)return -1;aligned=aligned/alignment*alignment;
    int64_t image=multiply(e,aligned,imageHeight?imageHeight:h);if(image<0)return -1;
    int64_t size=multiply(e,image,(int64_t)skipImages+d-1);if(size<0)return -1;
    size=add(e,size,multiply(e,aligned,(int64_t)skipRows+h-1));if(size<0)return -1;
    return add(e,size,multiply(e,bytes,(int64_t)skipPixels+w));
}
