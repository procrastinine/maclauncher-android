import maclauncher.build.UiHooks;
import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;
import java.util.*;

/** Class-file encoding of the shared platform callback declarations. */
final class UiHookClassFiles {
    static boolean apply(ClassNode node){
        boolean changed=false;
        for(var hook:UiHooks.UI){
            if(!hook.source().owner().equals(node.name))continue;
            MethodNode method=node.methods.stream().filter(value->value.name.equals(hook.source().name())&&value.desc.equals(hook.source().descriptor())).findFirst()
                .orElseThrow(()->new IllegalStateException("Android UI callback changed: "+hook.source()));
            if(hook.position()==UiHooks.Position.ENTRY)method.instructions.insert(call(node,method,hook,-1));
            else for(AbstractInsnNode instruction:method.instructions.toArray())if(instruction.getOpcode()>=Opcodes.IRETURN&&instruction.getOpcode()<=Opcodes.RETURN){
                Type result=Type.getReturnType(method.desc);int local=-1;InsnList code=new InsnList();
                if(result.getSort()!=Type.VOID){local=method.maxLocals;method.maxLocals+=result.getSize();code.add(new VarInsnNode(result.getOpcode(Opcodes.ISTORE),local));}
                code.add(call(node,method,hook,local));
                if(result.getSort()!=Type.VOID&&Type.getReturnType(hook.target().descriptor()).getSort()==Type.VOID)code.add(new VarInsnNode(result.getOpcode(Opcodes.ILOAD),local));
                method.instructions.insertBefore(instruction,code);
            }
            changed=true;
        }
        return changed;
    }
    private static InsnList call(ClassNode owner,MethodNode method,UiHooks.Hook hook,int resultLocal){
        List<Type> inputs=new ArrayList<>();if((method.access&Opcodes.ACC_STATIC)==0)inputs.add(Type.getObjectType(owner.name));inputs.addAll(Arrays.asList(Type.getArgumentTypes(method.desc)));
        int[] slots=new int[inputs.size()];for(int i=1;i<slots.length;i++)slots[i]=slots[i-1]+inputs.get(i-1).getSize();
        InsnList code=new InsnList();
        for(var value:hook.values()){
            var field=value.field();
            if(value.isStatic()){code.add(new FieldInsnNode(Opcodes.GETSTATIC,field.owner(),field.name(),field.descriptor()));continue;}
            Type type=value.parameter()==-1?Type.getReturnType(method.desc):inputs.get(value.parameter());
            code.add(new VarInsnNode(type.getOpcode(Opcodes.ILOAD),value.parameter()==-1?resultLocal:slots[value.parameter()]));
            if(field!=null)code.add(new FieldInsnNode(Opcodes.GETFIELD,field.owner(),field.name(),field.descriptor()));
        }
        var target=hook.target();code.add(new MethodInsnNode(Opcodes.INVOKESTATIC,target.owner(),target.name(),target.descriptor(),false));return code;
    }
}
