// Compact only methods whose ordinary translation exceeds the JVM code limit.
export function adaptArrayDataOutlining(source) {
  const replace=(before,after,count=1)=>{if(source.split(before).length!==count+1)throw Error('Pinned converter array-data boundary changed');source=source.replaceAll(before,after);};
  replace('        if (!allowOutlining) return;',`        if (!allowOutlining) return;
        if (done.mainWriter.measuredLength() > 65535 && done.hasArrayPayload()) {
            try {
                MethodTranslator arrays = translateOnce(method, code, mw, hierarchy,
                        repoints, superIfaces, null, null, true);
                arrays.commitSyntheticMethods();
                return;
            } catch (RuntimeException failure) {
                // No helper was attached before all bodies passed their limit.
                mw.newCode();
                done = translateOnce(method, code, mw, hierarchy, repoints, superIfaces,
                                     null, null);
            }
        }`);
  const once='                                                  MethodSplitter.Plan split) {\n        MethodTranslator base = translateBase(method, code, mw, hierarchy, repoints,\n                                              superIfaces, outline, split);';
  replace(once,`                                                  MethodSplitter.Plan split) {
        return translateOnce(method, code, mw, hierarchy, repoints, superIfaces,
                             outline, split, false);
    }

    private static MethodTranslator translateOnce(DexMethod method, DexCode code,
                                                  ClassFileWriter.MethodWriter mw,
                                                  DexType.ClassHierarchy hierarchy,
                                                  InitRepointPlan repoints,
                                                  SuperInterfacePlan superIfaces,
                                                  MethodOutliner.Plan outline,
                                                  MethodSplitter.Plan split,
                                                  boolean arrayHelpers) {
        MethodTranslator base = translateBase(method, code, mw, hierarchy, repoints,
                                              superIfaces, outline, split, arrayHelpers);`);
  replace('                                                  MethodSplitter.Plan split) {\n        // Under NARROW_TRY',
    '                                                  MethodSplitter.Plan split,\n                                                  boolean arrayHelpers) {\n        // Under NARROW_TRY');
  replace('                                                      null, null);\n            t.coverPlan',
    '                                                      null, null, arrayHelpers);\n            t.coverPlan');
  replace('                                                      false, repoints, superIfaces, outline, split);',
    '                                                      false, repoints, superIfaces, outline, split, arrayHelpers);',2);
  replace('                                                          true, repoints, superIfaces, outline, split);',
    '                                                          true, repoints, superIfaces, outline, split, arrayHelpers);');
  replace('        final List<Object[]> pendingSynthetics = new ArrayList<>();',
    '        final List<Object[]> pendingSynthetics = new ArrayList<>();\n        final boolean arrayHelpers;');
  replace('                         MethodOutliner.Plan outline, MethodSplitter.Plan split) {',
    '                         MethodOutliner.Plan outline, MethodSplitter.Plan split,\n                         boolean arrayHelpers) {\n            this.arrayHelpers = arrayHelpers;');
  replace('            if (split != null) {\n                int over = mainWriter.measuredLength();',
    '            if (split != null || arrayHelpers) {\n                int over = mainWriter.measuredLength();');
  replace('            for (int i = 0; i < count; i++) {\n                pushUse(it, 0);',`            if (arrayHelpers && count > 0) {
                emitArrayPayloadHelpers(insn, it, data, width, elem, storeOp, count);
                return;
            }
            for (int i = 0; i < count; i++) {
                pushUse(it, 0);`);
  replace('            for (String[] w : initializerHelperPuts) {',
    '            if (arrayHelpers && !pendingSynthetics.isEmpty()\n                    && method.declaringClass().isInterface()) {\n                mw.owner.setVersion(Math.max(mw.owner.majorVersion(), 53), 0);\n            }\n            for (String[] w : initializerHelperPuts) {');
  replace('        void pushArrayElement(byte[] data, int i, int width, String elem) {',`        boolean hasArrayPayload() {
            for (Instruction instruction : decoded) {
                if (instruction.opcode() == Opcodes.FILL_ARRAY_DATA
                        && instruction.arrayData() != null && instruction.arrayData().length > 0) return true;
            }
            return false;
        }

        void emitArrayPayloadHelpers(TypeInference.Insn insn, TypeInference.InsnTypes it,
                                     byte[] data, int width, String elem, int storeOp, int count) {
            java.util.Set<String> taken = new HashSet<>();
            for (DexMethod declared : method.declaringClass().methods()) taken.add(declared.name());
            for (Object[] pending : pendingSynthetics) taken.add((String) pending[0]);
            String descriptor = "([" + elem + ")V";
            // Even an eight-byte raw constant, a wide index and an array store
            // fit well below code_length in one chunk. No temporary array or
            // length check changes null, partial-write or bounds semantics.
            for (int begin = 0; begin < count; begin += 2048) {
                String name = Options.syntheticPrefix + "$array$" + method.methodIndex()
                        + "$" + insn.offset + "$" + begin;
                while (!taken.add(name)) name += "_";
                CodeWriter body = new CodeWriter(mw.owner.pool());
                body.setInitialFrame(StackMapWriter.initialFrame(ownerName, name, descriptor, true));
                body.setMaxLocals(1);
                CodeWriter parent = out;
                try {
                    out = body;
                    int end = Math.min(count, begin + 2048);
                    for (int i = begin; i < end; i++) {
                        out.varOp(CodeWriter.ALOAD, 0);
                        out.pushInt(i);
                        pushArrayElement(data, i, width, elem);
                        out.op(storeOp);
                    }
                    out.op(CodeWriter.RETURN);
                } finally {
                    out = parent;
                }
                pendingSynthetics.add(new Object[]{name, descriptor, body});
                pushUse(it, 0);
                out.methodOp(CodeWriter.INVOKESTATIC, ownerName, name, descriptor,
                             method.declaringClass().isInterface());
            }
        }

        void pushArrayElement(byte[] data, int i, int width, String elem) {`);
  return source;
}
