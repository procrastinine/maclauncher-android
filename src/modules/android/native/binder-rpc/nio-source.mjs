import {includeDependencies} from '../upstream/source-includes.mjs';
/** Original nativehelper algorithms with the JVM's actual buffer layout. */
export function adaptNioSource(source){
  const replace=(before,after)=>{
    if(source.split(before).length!==2)throw Error('Original JNI buffer storage boundary changed: '+before);
    source=source.replace(before,after);
  };
  for(const [field,name]of [['position','position'],['limit','limit'],['_elementSizeShift','shift']])
    replace('return(*env)->GetIntField(env, nioBuffer, JniConstants_NioBuffer_'+field+'(env));',
      'return klj_nio_get_'+name+'(env, nioBuffer);');
  replace(`    jclass nioAccessClass = JniConstants_NIOAccessClass(env);
    jmethodID getBaseArrayMethod = JniConstants_NIOAccess_getBaseArray(env);
    jobject object = (*env)->CallStaticObjectMethod(env,
                                                    nioAccessClass, getBaseArrayMethod, nioBuffer);
    return (jarray) object;`,'    return klj_nio_array(env, nioBuffer);');
  replace(`    jclass nioAccessClass = JniConstants_NIOAccessClass(env);
    jmethodID getBaseArrayOffsetMethod = JniConstants_NIOAccess_getBaseArrayOffset(env);
    return (*env)->CallStaticIntMethod(env, nioAccessClass, getBaseArrayOffsetMethod, nioBuffer);`,
    '    return klj_nio_array_offset(env, nioBuffer);');
  const address='(*env)->GetLongField(env, nioBuffer, JniConstants_NioBuffer_address(env))';
  if(source.split(address).length!==3)throw Error('Original JNI direct buffer address boundaries changed');
  return includeDependencies(source.replaceAll(address,'klj_nio_address(env, nioBuffer)'),'#include "../maclauncher/binder-transport/jvm-nio.h"');
}
