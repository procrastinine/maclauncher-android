import {adaptArrayDataOutlining} from './converter-array-source.mjs';

/** Preserve JVM receiver requirements and Dalvik result-discard semantics. */
export function adaptTranslator(source) {
  const before='            if (u.checkCastTo != null) return;   // the analysis already cast it\n            String have = u.type.ref().name;';
  const after='            // The inferred cast may name a superinterface while invokespecial\n'
    +'            // requires the current class. Refine the actual post-cast type.\n'
    +'            String have = u.checkCastTo != null ? u.checkCastTo : u.type.ref().name;';
  if(source.split(before).length!==2)throw new Error('The pinned converter receiver adaptation needs updating.');
  const arrayBefore='            // Leaves the array on the stack for the following move-result,\n'
    +'            // matching how invoke leaves its result.';
  const arrayAfter='            // Dalvik discards an unused filled-new-array result, just as\n'
    +'            // it discards an unused invocation result. Keep every ordinary\n'
    +'            // instruction boundary stack-neutral when no move-result follows.\n'
    +'            if (!nextIsMoveResult(insn)) out.op(CodeWriter.POP);';
  if(source.split(arrayBefore).length!==2)throw new Error('The pinned converter array-result boundary needs updating.');
  return adaptArrayDataOutlining(source.replace(before,after).replace(arrayBefore,arrayAfter));
}

/** Every converter entry point shares the exact same post-conversion JVM boundary. */
export function adaptConverter(source) {
  const before="        return cw.toByteArray();";
  if(source.split(before).length!==2)throw new Error("The pinned converter class-output boundary changed.");
  return source.replace(before,"        return new maclauncher.android.portability.PortableCalls(false).normalize(cw.toByteArray());");
}

/** ART resolves a constructor reference up the superclass chain even when the
 * reference names the allocated class. Reuse the upstream forwarding plan to
 * express that resolution in JVM bytecode, where constructors are not inherited. */
export function adaptInitRepointPlan(source) {
  const replace=(from,to)=>{if(source.split(from).length!==2)throw Error('The pinned constructor resolution boundary changed.');source=source.replace(from,to);};
  replace('                    if (!named.equals(ownerName) && !named.equals(superName)) {\n                        visitor.site(in.address(), true, null, ref);\n                    }','                    visitor.site(in.address(), true, null, ref);');
  replace('} else if (alloc != null && !alloc.equals(named)) {','} else if (alloc != null) {');
  replace('                    String desc = ref.proto().descriptor();',`                    String desc = ref.proto().descriptor();
                    // Ordinary declared constructors need no new plan. The
                    // newly admitted sites are only inherited constructor
                    // references that ART resolves beyond their named class.
                    boolean ordinary = owner.equals(allocated) || uninitThis &&
                            (owner.equals(c.name()) || owner.equals(c.superclassName()));
                    if (ordinary && owner.equals(constructorOwner(byName, owner, desc))) return;`);
  replace('b.plan(cand.start, cand.namedOwner, cand.desc, cand.referrers)',
          'b.plan(cand.start, constructorOwner(byName, cand.namedOwner, cand.desc), cand.desc, cand.referrers)');
  replace('    private static DexCode transientCode(DexMethod m) {',`    private static String constructorOwner(Map<String,DexClass> classes,String named,String descriptor) {
        java.util.HashSet<String> seen=new java.util.HashSet<>();
        String current=named;
        while(current!=null && seen.add(current)) {
            DexClass type=classes.get(current);
            if(type==null || type.isInterface()) return current;
            for(DexMethod method:type.methods())
                if("<init>".equals(method.name()) && descriptor.equals(method.descriptor())) return current;
            current=type.superclassName();
        }
        return named;
    }

    private static DexCode transientCode(DexMethod m) {`);
  return source;
}
