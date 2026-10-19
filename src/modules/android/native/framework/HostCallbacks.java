import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.jar.*;
import org.objectweb.asm.*;

/** Build-time callback bindings from the actual host/framework bytecode; no classes are loaded. */
public final class HostCallbacks {
    record Callback(String alias,String owner,String name,String descriptor,boolean isStatic){boolean constructor(){return name.equals("<init>");}}
    private final List<Path> inputs;
    private final LinkedHashMap<String,String> owners=new LinkedHashMap<>();
    private HostCallbacks(List<Path> inputs){this.inputs=inputs;}
    private byte[] bytes(String owner)throws IOException{
        String name=owner+".class";
        for(Path input:inputs){
            if(Files.isDirectory(input)){Path file=input.resolve(name);if(Files.isRegularFile(file))return Files.readAllBytes(file);}
            else try(JarFile jar=new JarFile(input.toFile())){var entry=jar.getJarEntry(name);if(entry!=null)try(var in=jar.getInputStream(entry)){return in.readAllBytes();}}
        }
        if(owner.startsWith("java/"))try(var in=ClassLoader.getSystemResourceAsStream(name)){if(in!=null)return in.readAllBytes();}
        throw new IOException("Callback declaring class is unavailable: "+owner);
    }
    private void verify(Callback callback)throws IOException{
        int[] access={-1};
        new ClassReader(bytes(callback.owner)).accept(new ClassVisitor(Opcodes.ASM9){
            @Override public MethodVisitor visitMethod(int flags,String name,String descriptor,String signature,String[] exceptions){
                if(name.equals(callback.name)&&descriptor.equals(callback.descriptor))access[0]=flags;
                return null;
            }
        },ClassReader.SKIP_CODE|ClassReader.SKIP_DEBUG|ClassReader.SKIP_FRAMES);
        String method=callback.owner+"."+callback.name+callback.descriptor;
        if(access[0]<0)throw new IllegalArgumentException("Callback declaration is unavailable: "+method);
        if(((access[0]&Opcodes.ACC_STATIC)!=0)!=callback.isStatic)throw new IllegalArgumentException("Callback staticness changed: "+method);
        if((access[0]&Opcodes.ACC_PUBLIC)==0)throw new IllegalArgumentException("Callback is not public: "+method);
    }
    private static String baseType(Type type){return switch(type.getSort()){
        case Type.VOID->"void";case Type.BOOLEAN->"jboolean";case Type.BYTE->"jbyte";
        case Type.CHAR->"jchar";case Type.SHORT->"jshort";case Type.INT->"jint";
        case Type.LONG->"jlong";case Type.FLOAT->"jfloat";case Type.DOUBLE->"jdouble";
        case Type.ARRAY->type.getDimensions()==1&&type.getElementType().getSort()<Type.ARRAY?baseType(type.getElementType())+"Array":"jobjectArray";
        case Type.OBJECT->type.getInternalName().equals("java/lang/String")?"jstring":type.getInternalName().equals("java/lang/Throwable")?"jthrowable":"jobject";
        default->throw new IllegalArgumentException(type.toString());
    };}
    private static String family(Type type){return switch(type.getSort()){
        case Type.VOID->"Void";case Type.BOOLEAN->"Boolean";case Type.BYTE->"Byte";case Type.CHAR->"Char";
        case Type.SHORT->"Short";case Type.INT->"Int";case Type.LONG->"Long";case Type.FLOAT->"Float";
        case Type.DOUBLE->"Double";default->"Object";
    };}
    private String argumentType(Type type){String typed=typedResult(type);return typed==null?baseType(type):typed;}
    private String typedResult(Type result){return result.getSort()==Type.OBJECT?owners.get(result.getInternalName()):null;}
    private record Generated(String header,String source){}
    private Generated generate(List<Callback> callbacks,String headerName){
        StringBuilder header=new StringBuilder("/* Generated from compiled declarations by HostCallbacks. Do not edit. */\n#pragma once\n#include <jni.h>\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n");
        for(String owner:owners.values())header.append("typedef struct { jobject object; } ").append(owner).append(";\n");
        header.append("typedef struct KLHostCallbacks KLHostCallbacks;\nKLHostCallbacks* kl_host_callbacks_create(JNIEnv*);\nKLHostCallbacks* kl_host_callbacks_retain(KLHostCallbacks*);\nvoid kl_host_callbacks_release(JNIEnv*,KLHostCallbacks*);\n");
        StringBuilder out=new StringBuilder("/* Generated callback implementation. Do not edit. */\n#include \"").append(headerName).append("\"\n#include <stdlib.h>\n#include <stdatomic.h>\nstruct KLHostCallbacks {\n atomic_uint references;\n");
        for(String owner:owners.values())out.append(" _Atomic(jclass) class_").append(owner).append(";\n");
        for(Callback callback:callbacks)out.append(" _Atomic(jmethodID) method_").append(callback.alias).append(";\n");
        out.append("};\n");
        for(var owner:owners.entrySet()){
            String type=owner.getValue();
            out.append("static jclass class_").append(type).append("(JNIEnv*env,KLHostCallbacks*callbacks){\n if(!callbacks||(*env)->ExceptionCheck(env))return NULL;\n jclass cached=atomic_load_explicit(&callbacks->class_").append(type).append(",memory_order_acquire);if(cached)return cached;\n jclass local=(*env)->FindClass(env,\"").append(owner.getKey()).append("\");if(!local)return NULL;\n jclass made=(*env)->NewGlobalRef(env,local);(*env)->DeleteLocalRef(env,local);if(!made)return NULL;\n if(!atomic_compare_exchange_strong_explicit(&callbacks->class_").append(type).append(",&cached,made,memory_order_release,memory_order_acquire)){(*env)->DeleteGlobalRef(env,made);return cached;}return made;\n}\n");
        }
        for(Callback c:callbacks)out.append("static jmethodID method_").append(c.alias).append("(JNIEnv*env,KLHostCallbacks*callbacks){\n jclass owner=class_").append(owners.get(c.owner)).append("(env,callbacks);if(!owner)return NULL;\n jmethodID cached=atomic_load_explicit(&callbacks->method_").append(c.alias).append(",memory_order_acquire);if(cached)return cached;\n jmethodID made=(*env)->Get").append(c.isStatic?"Static":"").append("MethodID(env,owner,\"").append(c.name).append("\",\"").append(c.descriptor).append("\");if(!made)return NULL;\n atomic_compare_exchange_strong_explicit(&callbacks->method_").append(c.alias).append(",&cached,made,memory_order_release,memory_order_acquire);return cached?cached:made;\n}\n");
        out.append("KLHostCallbacks* kl_host_callbacks_retain(KLHostCallbacks*callbacks){if(callbacks)atomic_fetch_add_explicit(&callbacks->references,1,memory_order_relaxed);return callbacks;}\nvoid kl_host_callbacks_release(JNIEnv *env,KLHostCallbacks *callbacks){\n if(!callbacks||atomic_fetch_sub_explicit(&callbacks->references,1,memory_order_acq_rel)!=1)return;\n");
        for(String owner:owners.values())out.append(" {jclass owner=atomic_load_explicit(&callbacks->class_").append(owner).append(",memory_order_relaxed);if(owner)(*env)->DeleteGlobalRef(env,owner);}\n");
        out.append(" free(callbacks);\n}\nKLHostCallbacks* kl_host_callbacks_create(JNIEnv *env){\n if((*env)->ExceptionCheck(env))return NULL;\n KLHostCallbacks* callbacks=calloc(1,sizeof(*callbacks));\n if(!callbacks){jclass error=(*env)->FindClass(env,\"java/lang/OutOfMemoryError\");if(error){(*env)->ThrowNew(env,error,\"Native callback allocation failed\");(*env)->DeleteLocalRef(env,error);}return NULL;}\n");
        out.append(" atomic_init(&callbacks->references,1);\n");
        for(String owner:owners.values())out.append(" atomic_init(&callbacks->class_").append(owner).append(",NULL);\n");
        for(Callback callback:callbacks)out.append(" atomic_init(&callbacks->method_").append(callback.alias).append(",NULL);\n");
        out.append(" return callbacks;\n}\n");
        for(var owner:owners.entrySet()){
            String type=owner.getValue();
            String declaration=type+" kl_host_bind_"+type+"(JNIEnv *env,KLHostCallbacks *callbacks,jobject object)";
            header.append(declaration).append(";\n");
            out.append(declaration).append("{\n")
               .append(" if(!callbacks||(*env)->ExceptionCheck(env))return (").append(type).append("){0};\n")
               .append(" if(!object)return (").append(type).append("){0};jclass owner=class_").append(type).append("(env,callbacks);if(!owner)return (").append(type).append("){0};\n")
               .append(" if(!(*env)->IsInstanceOf(env,object,owner)){jclass error=(*env)->FindClass(env,\"java/lang/IllegalArgumentException\");if(error){(*env)->ThrowNew(env,error,\"Callback receiver must be ").append(owner.getKey()).append("\");(*env)->DeleteLocalRef(env,error);}return (").append(type).append("){0};}\n")
               .append(" return (").append(type).append("){object};\n}\n");
        }
        for(Callback c:callbacks){
            Type result=c.constructor()?Type.getObjectType(c.owner):Type.getReturnType(c.descriptor);Type[] args=Type.getArgumentTypes(c.descriptor);
            String typed=typedResult(result),returnType=typed==null?baseType(result):typed;
            String zero=result.getSort()==Type.VOID?"":typed==null?" 0":" ("+typed+"){0}";
            StringBuilder declaration=new StringBuilder(returnType).append(" kl_host_").append(c.alias).append("(JNIEnv *env,KLHostCallbacks *callbacks");
            if(!c.isStatic&&!c.constructor())declaration.append(',').append(owners.get(c.owner)).append(" receiver");
            for(int i=0;i<args.length;i++)declaration.append(',').append(argumentType(args[i])).append(" arg").append(i);
            declaration.append(')');header.append(declaration).append(";\n");
            out.append(declaration).append("{\n if(!callbacks||(*env)->ExceptionCheck(env)");
            if(!c.isStatic&&!c.constructor())out.append("||!receiver.object");
            out.append(")return").append(zero).append(";\n");
            out.append(" jmethodID method=method_").append(c.alias).append("(env,callbacks);if(!method)return").append(zero).append(";\n");
            if(args.length>0){out.append(" jvalue args[").append(args.length).append("]={0};\n");for(int i=0;i<args.length;i++)out.append(" args[").append(i).append("].").append(args[i].getSort()>=Type.ARRAY?"l":args[i].getDescriptor().toLowerCase(Locale.ROOT)).append("=arg").append(i).append(typedResult(args[i])==null?"":".object").append(";\n");}
            String call="(*env)->"+(c.constructor()?"NewObjectA":"Call"+(c.isStatic?"Static":"")+family(result)+"MethodA")+"(env,"+(c.isStatic||c.constructor()?"callbacks->class_"+owners.get(c.owner):"receiver.object")+",method,"+(args.length>0?"args":"NULL")+")";
            if(typed!=null)out.append(" return (").append(typed).append("){").append(call).append("};\n");
            else out.append(result.getSort()==Type.VOID?" ":" return ("+returnType+")").append(call).append(";\n");
            out.append("}\n");
        }
        header.append("#ifdef __cplusplus\n}\n#endif\n");
        return new Generated(header.toString(),out.toString());
    }
    public static void main(String[] args)throws Exception{
        if(args.length<3)throw new IllegalArgumentException("HostCallbacks contract.tsv output.h classes-or-jars...");
        var generator=new HostCallbacks(Arrays.stream(args).skip(2).map(Path::of).toList());
        var callbacks=new ArrayList<Callback>();var names=new HashSet<String>();
        for(String line:Files.readAllLines(Path.of(args[0]))){
            if(line.isBlank()||line.startsWith("#"))continue;
            String[] f=line.split("\\t",-1);
            if(f.length!=5||!f[0].matches("[a-z][a-z_0-9]*")||!Set.of("static","instance","constructor").contains(f[4])||!names.add(f[0]))throw new IllegalArgumentException("Invalid callback contract: "+line);
            if(f[4].equals("constructor")!=f[2].equals("<init>"))throw new IllegalArgumentException("Constructor callback kind differs from declaration: "+line);
            var callback=new Callback(f[0],f[1],f[2],f[3],f[4].equals("static"));generator.verify(callback);callbacks.add(callback);
            generator.owners.putIfAbsent(f[1],"KLHost_"+f[1].replace('/','_').replace('$','_'));
            for(Type argument:Type.getArgumentTypes(f[3]))if(argument.getSort()==Type.OBJECT){String owner=argument.getInternalName();generator.owners.putIfAbsent(owner,"KLHost_"+owner.replace('/','_').replace('$','_'));}
        }
        Path header=Path.of(args[1]);String name=header.getFileName().toString();
        if(!name.matches("[a-zA-Z0-9_-]+\\.h"))throw new IllegalArgumentException("Callback output requires a C header filename");
        Generated generated=generator.generate(callbacks,name);
        Files.writeString(header,generated.header);
        Files.writeString(header.resolveSibling(name.substring(0,name.length()-2)+".c"),generated.source);
    }
}
