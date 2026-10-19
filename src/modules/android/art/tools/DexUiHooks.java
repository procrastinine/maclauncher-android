package maclauncher.build;

import com.android.tools.smali.dexlib2.*;
import com.android.tools.smali.dexlib2.builder.*;
import com.android.tools.smali.dexlib2.builder.instruction.*;
import com.android.tools.smali.dexlib2.iface.*;
import com.android.tools.smali.dexlib2.iface.instruction.OneRegisterInstruction;
import com.android.tools.smali.dexlib2.immutable.*;
import com.android.tools.smali.dexlib2.immutable.instruction.*;
import com.android.tools.smali.dexlib2.immutable.reference.*;
import java.util.*;

/** DEX encoding of the same UI callbacks used by the JVM build binding.
 * dexlib2 relocates original branches, handlers, payloads and debug positions. */
final class DexUiHooks {
    static List<String> inputs(Method method){
        List<String> result=new ArrayList<>();if((method.getAccessFlags()&AccessFlags.STATIC.getValue())==0)result.add(method.getDefiningClass());
        method.getParameterTypes().forEach(value->result.add(value.toString()));return result;
    }
    static Opcode move(String type){return type.startsWith("L")||type.startsWith("[")?Opcode.MOVE_OBJECT_16:DexBodies.width(type)==2?Opcode.MOVE_WIDE_16:Opcode.MOVE_16;}
    static ImmutableMethodReference reference(UiHooks.Member member){return DexBodies.member(member.owner().replace('/','.')+"."+member.name()+member.descriptor()).reference();}
    static List<UiHooks.Hook> hooks(Method method){return UiHooks.UI.stream().filter(hook->("L"+hook.source().owner()+";").equals(method.getDefiningClass())&&hook.source().name().equals(method.getName())&&hook.source().descriptor().equals("("+String.join("",method.getParameterTypes())+")"+method.getReturnType())).toList();}
    static String helperName(UiHooks.Hook hook){return "$maclauncher$ui$"+UiHooks.UI.indexOf(hook);}

    private static Method helper(Method original,UiHooks.Hook hook){
        var target=DexBodies.member(hook.target().owner().replace('/','.')+"."+hook.target().name()+hook.target().descriptor());
        List<String> parameters=new ArrayList<>();
        boolean result=hook.position()==UiHooks.Position.RETURN&&!original.getReturnType().equals("V");
        if(result)parameters.add(original.getReturnType());parameters.addAll(inputs(original));
        int words=target.parameters().stream().mapToInt(DexBodies::width).sum(),locals=words+1;
        int[] source=new int[parameters.size()];int register=locals;
        for(int i=0;i<source.length;i++){source[i]=register;register+=DexBodies.width(parameters.get(i));}
        List<com.android.tools.smali.dexlib2.iface.instruction.Instruction> code=new ArrayList<>();int destination=0;
        for(int i=0;i<hook.values().size();i++){
            var value=hook.values().get(i);String type=target.parameters().get(i);var field=value.field();
            if(value.isStatic())code.add(new ImmutableInstruction21c(type.startsWith("L")||type.startsWith("[")?Opcode.SGET_OBJECT:DexBodies.width(type)==2?Opcode.SGET_WIDE:Opcode.SGET,destination,new ImmutableFieldReference("L"+field.owner()+";",field.name(),field.descriptor())));
            else{
                int parameter=value.parameter()==-1?0:value.parameter()+(result?1:0);
                if(field==null)code.add(new ImmutableInstruction32x(move(type),destination,source[parameter]));
                else{
                    code.add(new ImmutableInstruction22x(Opcode.MOVE_OBJECT_FROM16,words,source[parameter]));
                    code.add(new ImmutableInstruction22c(type.startsWith("L")||type.startsWith("[")?Opcode.IGET_OBJECT:DexBodies.width(type)==2?Opcode.IGET_WIDE:Opcode.IGET,destination,words,new ImmutableFieldReference("L"+field.owner()+";",field.name(),field.descriptor())));
                }
            }
            destination+=DexBodies.width(type);
        }
        code.add(new ImmutableInstruction3rc(Opcode.INVOKE_STATIC_RANGE,0,words,target.reference()));
        if(target.result().equals("V"))code.add(new ImmutableInstruction10x(Opcode.RETURN_VOID));
        else{boolean object=target.result().startsWith("L")||target.result().startsWith("[");boolean wide=DexBodies.width(target.result())==2;
            code.add(new ImmutableInstruction11x(object?Opcode.MOVE_RESULT_OBJECT:wide?Opcode.MOVE_RESULT_WIDE:Opcode.MOVE_RESULT,0));
            code.add(new ImmutableInstruction11x(object?Opcode.RETURN_OBJECT:wide?Opcode.RETURN_WIDE:Opcode.RETURN,0));}
        var args=parameters.stream().map(type->new ImmutableMethodParameter(type,Set.of(),null)).toList();
        return new ImmutableMethod(original.getDefiningClass(),helperName(hook),args,target.result(),AccessFlags.PRIVATE.getValue()|AccessFlags.STATIC.getValue()|AccessFlags.SYNTHETIC.getValue(),Set.of(),Set.of(),new ImmutableMethodImplementation(register,code,List.of(),List.of()));
    }
    private static Method instrument(Method original,List<UiHooks.Hook> hooks,List<Method> helpers){
        var body=Objects.requireNonNull(original.getImplementation(),"Original UI method has no implementation");
        List<String> parameters=inputs(original);int words=parameters.stream().mapToInt(DexBodies::width).sum(),old=body.getRegisterCount();
        int result=hooks.stream().anyMatch(hook->hook.position()==UiHooks.Position.RETURN)?DexBodies.width(original.getReturnType()):0;
        // Original operand registers stay unchanged. Incoming parameters are
        // copied to their old slots; the retained incoming block also preserves
        // the receiver if the original compiler reuses a parameter register.
        var mutable=new MutableMethodImplementation(new ImmutableMethodImplementation(old+words+result,body.getInstructions(),body.getTryBlocks(),body.getDebugItems()));
        List<BuilderInstruction> originals=List.copyOf(mutable.getInstructions());
        Map<UiHooks.Hook,Method> adapters=new LinkedHashMap<>();for(var hook:hooks){Method adapter=helper(original,hook);adapters.put(hook,adapter);helpers.add(adapter);}
        for(int index=originals.size()-1;index>=0;index--){
            BuilderInstruction instruction=originals.get(index);Opcode opcode=instruction.getOpcode();
            if(opcode!=Opcode.RETURN&&opcode!=Opcode.RETURN_WIDE&&opcode!=Opcode.RETURN_OBJECT&&opcode!=Opcode.RETURN_VOID)continue;
            List<BuilderInstruction> before=new ArrayList<>();
            for(var hook:hooks)if(hook.position()==UiHooks.Position.RETURN){
                if(result!=0)before.add(new BuilderInstruction32x(move(original.getReturnType()),old,((OneRegisterInstruction)instruction).getRegisterA()));
                Method adapter=adapters.get(hook);before.add(new BuilderInstruction3rc(Opcode.INVOKE_STATIC_RANGE,old,words+result,new ImmutableMethodReference(original.getDefiningClass(),adapter.getName(),adapter.getParameterTypes(),adapter.getReturnType())));
                if(!adapter.getReturnType().equals("V"))before.add(new BuilderInstruction11x(original.getReturnType().startsWith("L")||original.getReturnType().startsWith("[")?Opcode.MOVE_RESULT_OBJECT:result==2?Opcode.MOVE_RESULT_WIDE:Opcode.MOVE_RESULT,((OneRegisterInstruction)instruction).getRegisterA()));
            }
            if(!before.isEmpty()){
                // Keep labels targeting the return on the first hook, so every
                // original return edge passes through its completion callback.
                mutable.replaceInstruction(index,before.getFirst());for(int n=1;n<before.size();n++)mutable.addInstruction(index+n,before.get(n));mutable.addInstruction(index+before.size(),instruction);
            }
        }
        List<BuilderInstruction> prefix=new ArrayList<>();int incoming=old+result,destination=old-words;
        for(String type:parameters){prefix.add(new BuilderInstruction32x(move(type),destination,incoming));destination+=DexBodies.width(type);incoming+=DexBodies.width(type);}
        for(var hook:hooks)if(hook.position()==UiHooks.Position.ENTRY){Method adapter=adapters.get(hook);prefix.add(new BuilderInstruction3rc(Opcode.INVOKE_STATIC_RANGE,old+result,words,new ImmutableMethodReference(original.getDefiningClass(),adapter.getName(),adapter.getParameterTypes(),adapter.getReturnType())));}
        for(int i=0;i<prefix.size();i++)mutable.addInstruction(i,prefix.get(i));
        return new ImmutableMethod(original.getDefiningClass(),original.getName(),original.getParameters(),original.getReturnType(),original.getAccessFlags(),original.getAnnotations(),original.getHiddenApiRestrictions(),mutable);
    }
    static ClassDef apply(ClassDef original){
        String type=original.getType();
        List<UiHooks.Hook> expected=UiHooks.UI.stream().filter(hook->("L"+hook.source().owner()+";").equals(type)).toList();
        if(expected.isEmpty())return original;
        original=ImmutableClassDef.of(original);
        List<Method> methods=new ArrayList<>(),helpers=new ArrayList<>();Set<UiHooks.Hook> found=new HashSet<>();
        for(Method method:original.getMethods()){
            if(method.getName().startsWith("$maclauncher$ui$"))throw new IllegalArgumentException("UI callbacks were already bound: "+original.getType());
            var hooks=hooks(method);found.addAll(hooks);methods.add(hooks.isEmpty()?method:instrument(method,hooks,helpers));
        }
        if(!found.containsAll(expected))throw new IllegalArgumentException("Original Android UI callback signatures changed: "+original.getType());
        methods.addAll(helpers);
        return new ImmutableClassDef(original.getType(),original.getAccessFlags(),original.getSuperclass(),original.getInterfaces(),original.getSourceFile(),original.getAnnotations(),original.getFields(),methods);
    }
}
