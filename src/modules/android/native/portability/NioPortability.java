package maclauncher.android.portability;

import java.util.*;
import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;
import org.objectweb.asm.tree.analysis.*;

/** Android mapped-buffer construction uses the JVM's actual mapped-memory owner. */
final class NioPortability implements Opcodes {
 static final String TYPE="java/nio/DirectByteBuffer",HOST="maclauncher/android/framework/HostMappedBuffer";
 static final String ACCESS="maclauncher/android/framework/HostNio";
 static final Set<String> VIEWS=Set.of("slice","duplicate","asReadOnlyBuffer","asCharBuffer","asShortBuffer","asIntBuffer","asLongBuffer","asFloatBuffer","asDoubleBuffer");
 static final String CONSTRUCTOR="(IJLjava/io/FileDescriptor;Ljava/lang/Runnable;Z)V";
 static byte[] transform(byte[] bytes){
  var reader=new ClassReader(bytes);boolean references=false;var characters=new char[reader.getMaxStringLength()];
  for(int i=1;i<reader.getItemCount();i++){int item=reader.getItem(i);if(item!=0&&reader.readByte(item-1)==7){String name=reader.readUTF8(item,characters);if(name.contains("java/nio/")&&name.contains("Buffer")){references=true;break;}}}
  if(!references)return bytes;
  var type=new ClassNode(ASM9);reader.accept(type,0);boolean changed=false;
  var nativeMethods=new HashSet<String>();
  for(var method:type.methods)if((method.access&ACC_NATIVE)!=0)nativeMethods.add(method.name+method.desc);
  for(var method:type.methods){
   // SourceInterpreter below visits this same method before ClassWriter has
   // recomputed its final maximum. Scope attachment adds one temporary copy.
   method.maxStack++;
   for(var instruction:method.instructions.toArray())if(instruction instanceof MethodInsnNode call){
    boolean buffer=call.owner.startsWith("java/nio/")&&call.owner.endsWith("Buffer");
    if(buffer&&call.getOpcode()==INVOKEVIRTUAL&&
       (call.name.equals("setAccessible")&&call.desc.equals("(Z)V")||call.name.equals("isAccessible")&&call.desc.equals("()Z"))){
     method.instructions.set(call,new MethodInsnNode(INVOKESTATIC,ACCESS,call.name,"(Ljava/nio/ByteBuffer;"+call.desc.substring(1),false));changed=true;continue;
    }
    if(buffer&&call.getOpcode()==INVOKEVIRTUAL&&VIEWS.contains(call.name)){
     // Attach a real shared JDK scope before a view is made. That view and all
     // earlier siblings inherit it even if accessibility is first changed later.
     Type[] arguments=Type.getArgumentTypes(call.desc);int[] locals=new int[arguments.length];var before=new InsnList();
     for(int n=arguments.length-1;n>=0;n--){locals[n]=method.maxLocals;method.maxLocals+=arguments[n].getSize();before.add(new VarInsnNode(arguments[n].getOpcode(ISTORE),locals[n]));}
     before.add(new InsnNode(DUP));before.add(new MethodInsnNode(INVOKESTATIC,ACCESS,"track","(Ljava/nio/Buffer;)V",false));
     for(int n=0;n<arguments.length;n++)before.add(new VarInsnNode(arguments[n].getOpcode(ILOAD),locals[n]));
     method.instructions.insertBefore(call,before);changed=true;
    }
    boolean allocation=call.getOpcode()==INVOKESTATIC&&call.owner.equals("java/nio/ByteBuffer")&&call.name.equals("allocateDirect");
    boolean nativeResult=call.owner.equals(type.name)&&nativeMethods.contains(call.name+call.desc);
    if(allocation||nativeResult){
     String result=Type.getReturnType(call.desc).getDescriptor(),operation=null,descriptor=null;
     if(result.equals("[Ljava/nio/ByteBuffer;")){operation="trackBuffers";descriptor="([Ljava/nio/ByteBuffer;)V";}
     else if(result.startsWith("Ljava/nio/")&&result.endsWith("Buffer;")){operation="track";descriptor="(Ljava/nio/Buffer;)V";}
     if(operation!=null){var after=new InsnList();after.add(new InsnNode(DUP));after.add(new MethodInsnNode(INVOKESTATIC,ACCESS,operation,descriptor,false));method.instructions.insert(call,after);changed=true;}
    }
   }
   for(var instruction:method.instructions.toArray())if(instruction instanceof TypeInsnNode test&&test.getOpcode()==INSTANCEOF&&test.desc.equals(TYPE)){
    // Android exposes DirectByteBuffer publicly; HotSpot's implementation is
    // private. Test the same direct-buffer property through its public API.
    method.instructions.set(test,new MethodInsnNode(INVOKESTATIC,"maclauncher/android/framework/HostNio","isDirectByteBuffer","(Ljava/lang/Object;)Z",false));changed=true;
   }
   var constructors=new ArrayList<MethodInsnNode>();
   for(var instruction:method.instructions)if(instruction instanceof MethodInsnNode call&&call.getOpcode()==INVOKESPECIAL&&call.owner.equals(TYPE)&&call.name.equals("<init>")&&call.desc.equals(CONSTRUCTOR))constructors.add(call);
   if(constructors.isEmpty())continue;
   // Follow stack/local copies back to their allocation; nested constructor
   // arguments must never select an unrelated NEW instruction.
   var interpreter=new SourceInterpreter(ASM9){@Override public SourceValue copyOperation(AbstractInsnNode instruction,SourceValue value){return value;}};
   Frame<SourceValue>[] frames;
   try{frames=new Analyzer<>(interpreter).analyze(type.name,method);}catch(AnalyzerException failure){throw new IllegalArgumentException("Cannot adapt mapped-buffer construction in "+type.name+"."+method.name,failure);}
   var rewrites=new ArrayList<AbstractInsnNode[]>();
   for(var call:constructors){
    var frame=frames[method.instructions.indexOf(call)];if(frame==null)continue;
    var receiver=frame.getStack(frame.getStackSize()-Type.getArgumentTypes(call.desc).length-1);
    if(receiver.insns.size()!=1)continue;var allocation=receiver.insns.iterator().next();
    if(!(allocation instanceof TypeInsnNode created)||created.getOpcode()!=NEW||!created.desc.equals(TYPE))continue;
    var duplicate=allocation.getNext();while(duplicate!=null&&duplicate.getOpcode()<0)duplicate=duplicate.getNext();
    if(duplicate==null||duplicate.getOpcode()!=DUP)continue;
    rewrites.add(new AbstractInsnNode[]{allocation,duplicate,call});
   }
   for(var rewrite:rewrites){
    method.instructions.remove(rewrite[0]);method.instructions.remove(rewrite[1]);
    var call=new MethodInsnNode(INVOKESTATIC,HOST,"create",CONSTRUCTOR.substring(0,CONSTRUCTOR.length()-1)+"Ljava/nio/MappedByteBuffer;",false);
    method.instructions.set(rewrite[2],call);
    var after=new InsnList();after.add(new InsnNode(DUP));after.add(new MethodInsnNode(INVOKESTATIC,ACCESS,"track","(Ljava/nio/Buffer;)V",false));method.instructions.insert(call,after);changed=true;
   }
   if(!rewrites.isEmpty())for(var instruction:method.instructions)if(instruction instanceof FrameNode frame){publicBufferTypes(frame.local);publicBufferTypes(frame.stack);}
  }
  if(!changed)return bytes;var writer=new ClassWriter(reader,ClassWriter.COMPUTE_MAXS);type.accept(writer);return writer.toByteArray();
 }
 private static void publicBufferTypes(List<Object> types){if(types!=null)for(int i=0;i<types.size();i++)if(TYPE.equals(types.get(i)))types.set(i,"java/nio/MappedByteBuffer");}
}
