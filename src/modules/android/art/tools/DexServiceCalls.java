package maclauncher.build;

import com.android.tools.smali.dexlib2.*;
import com.android.tools.smali.dexlib2.iface.*;
import com.android.tools.smali.dexlib2.iface.instruction.*;
import com.android.tools.smali.dexlib2.iface.reference.MethodReference;
import com.android.tools.smali.dexlib2.immutable.*;
import com.android.tools.smali.dexlib2.immutable.instruction.*;
import java.util.*;

/** Same service call binding as classfiles. Receiver and all original argument
 * registers stay in place; instruction widths, handlers and labels are intact. */
final class DexServiceCalls {
    static DexBodies.Member target(Method caller,Instruction instruction){
        Opcode opcode=instruction.getOpcode();
        if(opcode!=Opcode.INVOKE_VIRTUAL&&opcode!=Opcode.INVOKE_VIRTUAL_RANGE&&opcode!=Opcode.INVOKE_INTERFACE&&opcode!=Opcode.INVOKE_INTERFACE_RANGE)return null;
        var call=(MethodReference)((ReferenceInstruction)instruction).getReference();
        String owner=call.getDefiningClass(),type=caller.getDefiningClass();
        if(!PendingIntentCalls.binds(type.substring(1,type.length()-1),caller.getName(),owner.substring(1,owner.length()-1),call.getName()))return null;
        String descriptor="("+String.join("",call.getParameterTypes())+")"+call.getReturnType();
        return DexBodies.member(PendingIntentCalls.TARGET.replace('/','.')+"."+call.getName()+PendingIntentCalls.descriptor(descriptor));
    }
    static Method apply(Method method,Collection<DexBodies.Member> targets){
        var body=method.getImplementation();if(body==null||!PendingIntentCalls.applies(method.getDefiningClass().substring(1,method.getDefiningClass().length()-1)))return method;
        List<Instruction> instructions=new ArrayList<>();boolean changed=false;
        for(Instruction original:body.getInstructions()){
            var target=target(method,original);Instruction instruction=original;
            if(target!=null){changed=true;targets.add(target);
                if(original instanceof FiveRegisterInstruction call)instruction=new ImmutableInstruction35c(Opcode.INVOKE_STATIC,call.getRegisterCount(),call.getRegisterC(),call.getRegisterD(),call.getRegisterE(),call.getRegisterF(),call.getRegisterG(),target.reference());
                else{var call=(RegisterRangeInstruction)original;instruction=new ImmutableInstruction3rc(Opcode.INVOKE_STATIC_RANGE,call.getStartRegister(),call.getRegisterCount(),target.reference());}
            }
            instructions.add(instruction);
        }
        if(!changed)return method;
        return new ImmutableMethod(method.getDefiningClass(),method.getName(),method.getParameters(),method.getReturnType(),method.getAccessFlags(),method.getAnnotations(),method.getHiddenApiRestrictions(),new ImmutableMethodImplementation(body.getRegisterCount(),instructions,body.getTryBlocks(),body.getDebugItems()));
    }
}
