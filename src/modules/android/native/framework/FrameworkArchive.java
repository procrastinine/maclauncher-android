import maclauncher.android.portability.PortableCalls;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.*;
import java.security.MessageDigest;

/** Full executable AOSP framework with narrow JVM/libcore callsite normalization. */
public final class FrameworkArchive {
 // These exact donor entries are optional third-party compilation
 // placeholders, not Android boot classes. Keep their original declarations
 // as metadata without shadowing an application's complete compression SDK.
 static final String COMPILE_ONLY="META-INF/maclauncher/compile-only/";
 private static final Map<String,String> COMPILE_PLACEHOLDERS=Map.of(
  "com/github/luben/zstd/ZstdInputStream.class","d653e73c0d219cce2f08d7b78502f61e954211281141b5e42fb9f86f50f4defb",
  "com/github/luben/zstd/ZstdOutputStream.class","36ec5e209b07efd8f3795048cc62892da229eb8ce37ae67788aa6a0bcc294664",
  "org/tukaani/xz/LZMAOutputStream.class","e4ad65c31283cd632cd3168ac04345f2f908e2c402f09696470fa600ef498ba1");
 private static boolean compileOnly(ZipFile input,ZipEntry entry)throws Exception {
  String expected=COMPILE_PLACEHOLDERS.get(entry.getName());if(expected==null)return false;
  try(var stream=input.getInputStream(entry)){
   return expected.equals(HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(stream.readAllBytes())));
  }
 }
 public static void main(String[] args)throws Exception {
  if(args.length<2)throw new IllegalArgumentException("Expected framework input, output and optional module JARs");
  int classes=0,changed=0;long bytes=0;PortableCalls portability=new PortableCalls();
  var inputs=new ArrayList<ZipFile>();
  try{
   for(int i=0;i<args.length;i++)if(i!=1)inputs.add(new ZipFile(args[i]));
   // A class has one implementation owner. Identical publisher copies can be
   // coalesced, but archive ordering must never select between different APIs
   // or bodies. Resolve composition before creating the output archive.
   Map<String,ZipFile> owners=new HashMap<>();
   Map<String,LinkedHashSet<String>> services=new HashMap<>();
   for(var input:inputs)for(var entry:Collections.list(input.entries())){
    String name=entry.getName();
    if(!entry.isDirectory()&&name.startsWith("META-INF/services/")){
     var providers=services.computeIfAbsent(name,key->new LinkedHashSet<>());
     try(var stream=input.getInputStream(entry)){
      for(String line:new String(stream.readAllBytes(),StandardCharsets.UTF_8).split("\\R")){
       int comment=line.indexOf('#');String provider=(comment<0?line:line.substring(0,comment)).trim();
       if(!provider.isEmpty())providers.add(provider);
      }
     }
    }
    if(entry.isDirectory()||!name.endsWith(".class")||name.startsWith("native/")||name.startsWith("java/")||compileOnly(input,entry))continue;
    var previous=owners.putIfAbsent(name,input);
    if(previous!=null){
     try(var first=previous.getInputStream(previous.getEntry(name));var next=input.getInputStream(entry)){
      if(!Arrays.equals(first.readAllBytes(),next.readAllBytes()))
       throw new IllegalArgumentException("Conflicting framework class owners for "+name+": "+previous.getName()+" and "+input.getName());
     }
    }
   }
   try(var output=new ZipOutputStream(Files.newOutputStream(Path.of(args[1]),StandardOpenOption.CREATE_NEW))){
   DexPortability.initialize(inputs);
   GraphicsPortability.initialize(inputs,(name,original)->adaptBoundaries(name,original,new PortableCalls()));
   output.setLevel(6);
   Set<String> written=new HashSet<>();
   for(var input:inputs){
   for(var entry:Collections.list(input.entries()).stream().sorted(Comparator.comparing(ZipEntry::getName)).toList()){
    String name=entry.getName();
    // Linux JNI is incompatible with macOS; java.* belongs to the JDK module.
    // Framework classes retain their implementations. The donor's exact
    // third-party compile placeholders retain their bytes outside class lookup.
    if(entry.isDirectory()||name.startsWith("native/")||name.startsWith("java/"))continue;
    if(name.startsWith("/")||name.contains("../"))throw new IllegalArgumentException("Invalid framework archive entry");
    // Different class implementations were resolved above. ServiceLoader
    // declarations preserve all suppliers even when their original archives
    // use the same service configuration filename.
    boolean declarationOnly=compileOnly(input,entry);
    String outputName=declarationOnly?COMPILE_ONLY+name:name;
    if(!written.add(outputName))continue;
    var target=new ZipEntry(outputName);target.setTime(0);output.putNextEntry(target);
    try(var stream=input.getInputStream(entry)){
     if(services.containsKey(name)){
      byte[] providers=(String.join("\n",services.get(name))+"\n").getBytes(StandardCharsets.UTF_8);
      output.write(providers);bytes+=providers.length;
     }else if(name.endsWith(".class")&&!declarationOnly){
      byte[] original=stream.readAllBytes(),normalized=adapt(name,original,portability);
      if(normalized!=original)changed++;output.write(normalized);bytes+=normalized.length;classes++;
     }else bytes+=stream.transferTo(output);
    }
    output.closeEntry();
   }
   }
  }
  }finally{for(var input:inputs)input.close();}
  System.out.println("AOSP framework: "+classes+" classes, "+bytes+" uncompressed bytes, "+changed+" classes with JVM portability calls");
  for(var entry:portability.counts.entrySet())System.out.println("  "+entry.getKey()+" = "+entry.getValue());
 }
 private static byte[] adapt(String name,byte[] bytes,PortableCalls calls){
  return GraphicsPortability.transform(name,adaptBoundaries(name,bytes,calls));
 }
 private static byte[] adaptBoundaries(String name,byte[] bytes,PortableCalls calls){
  byte[] adapted=NativeBindings.transform(bytes);
  adapted=GlesPortability.transform(name,adapted);
  adapted=calls.normalize(adapted);
  adapted=SurfacePortability.transform(name,adapted);
  adapted=ResourcePortability.transform(name,adapted);
  adapted=CharsetPortability.transform(name,adapted);
  adapted=FilesystemPortability.transform(name,adapted);
  adapted=DescriptorPortability.transform(name,adapted);
  adapted=ServicePortability.transform(name,adapted);
  adapted=MessageQueuePortability.transform(name,adapted);
  adapted=UiPortability.transform(name,adapted);
  adapted=DexPortability.transform(name,adapted);
  return TaskPortability.transform(name,adapted);
 }

}
