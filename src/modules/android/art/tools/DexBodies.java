package maclauncher.build;

import com.android.tools.smali.dexlib2.*;
import com.android.tools.smali.dexlib2.iface.*;
import com.android.tools.smali.dexlib2.iface.instruction.Instruction;
import com.android.tools.smali.dexlib2.immutable.*;
import com.android.tools.smali.dexlib2.immutable.instruction.*;
import com.android.tools.smali.dexlib2.immutable.reference.ImmutableMethodReference;
import com.android.tools.smali.dexlib2.rewriter.*;
import com.android.tools.smali.dexlib2.writer.io.MemoryDataStore;
import com.android.tools.smali.dexlib2.writer.pool.DexPool;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** Build-time exact platform delegation. dexlib2 owns DEX layout and metadata. */
public final class DexBodies {
    record Member(String owner,String name,List<String> parameters,String result) {
        String key(){return owner+"->"+name+"("+String.join("",parameters)+")"+result;}
        ImmutableMethodReference reference(){return new ImmutableMethodReference(owner,name,parameters,result);}
    }
    static Member member(String text){
        int open=text.indexOf('('),close=text.indexOf(')',open);
        int split=text.lastIndexOf('.',open);
        if(open<0||close<0||split<1||close==text.length()-1)throw new IllegalArgumentException("Invalid method descriptor: "+text);
        String owner="L"+text.substring(0,split).replace('.','/')+";";
        List<String> parameters=new ArrayList<>();
        for(int at=open+1;at<close;){int start=at;while(text.charAt(at)=='[')at++;char kind=text.charAt(at++);
            if(kind=='L'){int end=text.indexOf(';',at);if(end<at||end>=close)throw new IllegalArgumentException("Invalid reference descriptor");at=end+1;}
            else if("ZBCSIFJD".indexOf(kind)<0)throw new IllegalArgumentException("Invalid parameter descriptor");
            parameters.add(text.substring(start,at));
        }
        return new Member(owner,text.substring(split+1,open),List.copyOf(parameters),text.substring(close+1));
    }
    static String key(Method method){return method.getDefiningClass()+"->"+method.getName()+"("+String.join("",method.getParameterTypes())+")"+method.getReturnType();}
    static Map<String,Member> mappings(Path file)throws IOException{
        Map<String,Member> result=new LinkedHashMap<>();
        for(String row:Files.readAllLines(file)){String line=row.strip();if(line.isEmpty()||line.startsWith("#"))continue;
            String[] parts=line.split("\\s+");if(parts.length!=2)throw new IllegalArgumentException("Expected original-method target-method: "+line);
            Member original=member(parts[0]),target=member(parts[1]);
            if(!original.result.equals(target.result))throw new IllegalArgumentException("Delegation return types differ: "+line);
            if(result.putIfAbsent(original.key(),target)!=null)throw new IllegalArgumentException("Duplicate delegation: "+parts[0]);
        }
        return result;
    }
    static void targets(Path jar,Collection<Member> targets)throws IOException{
        Set<String> pending=new HashSet<>();for(Member target:targets)pending.add(target.key());
        var container=DexFileFactory.loadDexContainer(jar.toFile(),null);
        for(String entry:container.getDexEntryNames())for(ClassDef type:container.getEntry(entry).getDexFile().getClasses())for(Method method:type.getMethods()){
            if(pending.contains(key(method))){int flags=AccessFlags.PUBLIC.getValue()|AccessFlags.STATIC.getValue();
                if((type.getAccessFlags()&AccessFlags.PUBLIC.getValue())==0||(method.getAccessFlags()&flags)!=flags)throw new IllegalArgumentException("Delegation target is not public static: "+key(method));
                pending.remove(key(method));
            }
        }
        if(!pending.isEmpty())throw new IllegalArgumentException("Delegation targets absent: "+pending);
    }
    static int width(String type){return type.equals("V")?0:type.equals("J")||type.equals("D")?2:1;}
    static Method delegate(Method original,Member target){
        if(original.getImplementation()==null)throw new IllegalArgumentException("Delegated method has no body: "+key(original));
        List<String> arguments=new ArrayList<>();
        if((original.getAccessFlags()&AccessFlags.STATIC.getValue())==0)arguments.add(original.getDefiningClass());
        for(CharSequence parameter:original.getParameterTypes())arguments.add(parameter.toString());
        if(!arguments.equals(target.parameters))throw new IllegalArgumentException("Delegation arguments differ: "+key(original));
        int inputs=arguments.stream().mapToInt(DexBodies::width).sum(),registers=Math.max(inputs,width(target.result));
        List<Instruction> instructions=new ArrayList<>();
        instructions.add(new ImmutableInstruction3rc(Opcode.INVOKE_STATIC_RANGE,0,inputs,target.reference()));
        if(target.result.equals("V"))instructions.add(new ImmutableInstruction10x(Opcode.RETURN_VOID));
        else{boolean object=target.result.startsWith("L")||target.result.startsWith("[");boolean wide=width(target.result)==2;
            instructions.add(new ImmutableInstruction11x(object?Opcode.MOVE_RESULT_OBJECT:wide?Opcode.MOVE_RESULT_WIDE:Opcode.MOVE_RESULT,0));
            instructions.add(new ImmutableInstruction11x(object?Opcode.RETURN_OBJECT:wide?Opcode.RETURN_WIDE:Opcode.RETURN,0));
        }
        var body=new ImmutableMethodImplementation(registers,instructions,List.of(),List.of());
        return new ImmutableMethod(original.getDefiningClass(),original.getName(),original.getParameters(),original.getReturnType(),original.getAccessFlags(),original.getAnnotations(),original.getHiddenApiRestrictions(),body);
    }
    static Map<String,byte[]> rewrite(Path input,Map<String,Member> mappings,Collection<Member> callTargets)throws IOException{
        var container=DexFileFactory.loadDexContainer(input.toFile(),null);
        Map<String,byte[]> rewritten=new HashMap<>();Set<String> pending=new HashSet<>(mappings.keySet());
        for(String entry:container.getDexEntryNames()){
            DexFile source=container.getEntry(entry).getDexFile();boolean affected=false;
            for(ClassDef type:source.getClasses())for(Method method:type.getMethods()){
                if(!DexUiHooks.hooks(method).isEmpty())affected=true;
                if(PendingIntentCalls.applies(type.getType().substring(1,type.getType().length()-1)))affected=true;
                if(mappings.containsKey(key(method))){affected=true;if(!pending.remove(key(method)))throw new IllegalArgumentException("Original method is duplicated: "+key(method));}
            }
            if(!affected)continue;
            DexRewriter rewriter=new DexRewriter(new RewriterModule(){
                @Override public Rewriter<ClassDef> getClassDefRewriter(Rewriters rewriters){
                    var original=super.getClassDefRewriter(rewriters);return type->DexUiHooks.apply(original.rewrite(type));
                }
                @Override public Rewriter<Method> getMethodRewriter(Rewriters rewriters){return method->{
                    String key=key(method);Member target=mappings.get(key);if(target==null)return DexServiceCalls.apply(method,callTargets);
                    return delegate(method,target);
                };}
            });
            MemoryDataStore store=new MemoryDataStore();try{
                DexPool.writeTo(store,rewriter.getDexFileRewriter().rewrite(source));rewritten.put(entry,store.getData());
            }finally{store.close();}
        }
        if(!pending.isEmpty())throw new IllegalArgumentException("Original methods absent: "+pending);
        return rewritten;
    }
    static void jar(Path input,Path output,Map<String,byte[]> replacements)throws IOException{
        // Open inputs before creating output. CREATE_NEW never touches a prior
        // artifact; cleanup applies only after this invocation created it.
        try(ZipFile source=new ZipFile(input.toFile())){
         OutputStream bytes=Files.newOutputStream(output,StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);
         try(ZipOutputStream zip=new ZipOutputStream(bytes)){
            var entries=source.entries();while(entries.hasMoreElements()){
                ZipEntry original=entries.nextElement();byte[] data=replacements.get(original.getName());
                if(data==null)try(InputStream stream=source.getInputStream(original)){data=stream.readAllBytes();}
                ZipEntry entry=new ZipEntry(original.getName());entry.setTime(original.getTime());entry.setComment(original.getComment());entry.setExtra(original.getExtra());entry.setMethod(original.getMethod());
                if(entry.getMethod()==ZipEntry.STORED){CRC32 crc=new CRC32();crc.update(data);entry.setSize(data.length);entry.setCompressedSize(data.length);entry.setCrc(crc.getValue());}
                zip.putNextEntry(entry);zip.write(data);zip.closeEntry();
            }
         }catch(Throwable failure){Files.deleteIfExists(output);throw failure;}
        }
    }
    public static void main(String[] arguments)throws Exception{
        Map<String,Path> options=new HashMap<>();for(int i=0;i<arguments.length;i+=2){if(i+1==arguments.length||!Set.of("--input","--bodies","--targets","--out").contains(arguments[i])||options.putIfAbsent(arguments[i],Path.of(arguments[i+1]))!=null)throw new IllegalArgumentException("Expected --input JAR --bodies TABLE --targets JAR --out JAR");}
        for(String name:List.of("--input","--bodies","--targets","--out"))Objects.requireNonNull(options.get(name),name);
        var mappings=mappings(options.get("--bodies"));targets(options.get("--targets"),mappings.values());
        Collection<Member> callTargets=new ArrayList<>();var replacements=rewrite(options.get("--input"),mappings,callTargets);
        targets(options.get("--targets"),callTargets);
        jar(options.get("--input"),options.get("--out"),replacements);
    }
}
