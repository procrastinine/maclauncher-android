package maclauncher.android;

import java.io.*;
import java.lang.ref.Cleaner;
import java.net.*;
import java.nio.*;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.security.*;
import java.time.Instant;
import java.util.*;
import java.util.jar.JarFile;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.locks.ReentrantLock;
import java.util.zip.*;

/** Local DEX conversion owned by the dedicated Android process. */
public final class DynamicDex {
    private static final long CACHE_TARGET=128L*1024*1024;
    private static final long PLATFORM_HEADER_TARGET=4L*1024*1024;
    private static final long PERSISTENT_CACHE_TARGET=256L*1024*1024;
    private static final java.util.regex.Pattern PERSISTENT_ENTRY=java.util.regex.Pattern.compile("[a-f0-9]{64}-[a-f0-9]{64}");
    private static final java.util.regex.Pattern SESSION_ENTRY=java.util.regex.Pattern.compile("session-[0-9]+");
    private static final Cleaner CLEANER=Cleaner.create();
    private static volatile Session current;
    private DynamicDex(){}

    public static synchronized void configure(Path runtime,Path cache,Path application,ClassLoader loader)throws IOException{
        if(current!=null)throw new IllegalStateException("Dynamic DEX is already configured");
        current=new Session(runtime.toRealPath(),cache.toRealPath(),application.toRealPath(),Objects.requireNonNull(loader));
    }
    /** Cancels conversion; existing class sources remain valid until their owners are collected. */
    public static void hostShutdown(){Session session=current;if(session!=null)session.shutdown();}
    /** The installed application's conversion is prepared before this JVM starts. */
    public static Module prepared(Path classes)throws IOException{return new Module(classes==null?null:classes.toRealPath());}
    public static Module prepared(Path classes,Set<String> members)throws IOException{return new Module(classes.toRealPath(),members);}
    /** The original AOSP loader requests the indexed class bytes individually.
     * Converter code and ASM remain in an owned subprocess, not this JVM. */
    public static Module demand(Path index,List<Path> sources,ClassLoader parent,Set<String> members)throws IOException{return new Module(index.toRealPath(),sources,parent,members);}
    public static boolean isDemand(Path index)throws IOException{
        try(var jar=new JarFile(index.toFile())){return jar.getJarEntry("META-INF/maclauncher/demand-dex")!=null;}
    }
    public static String[] preparedClassNames(Path index)throws IOException{try(var module=new Module(index.toRealPath(),(Set<String>)null)){return module.classNames();}}

    public static Module buffers(ByteBuffer[] inputs,ClassLoader parent){
        return snapshotBuffers(inputs,parent);
    }
    private static Module snapshotBuffers(ByteBuffer[] inputs,ClassLoader parent){
        Objects.requireNonNull(inputs);
        List<byte[]> bytes=new ArrayList<>();
        for(ByteBuffer input:inputs){ByteBuffer view=Objects.requireNonNull(input).duplicate();byte[] data=new byte[view.remaining()];view.get(data);bytes.add(data);}
        return convert(bytes,parent,List.of());
    }
    public static Module paths(String dexPath,ClassLoader parent){
        return snapshotPaths(dexPath,parent);
    }
    private static Module snapshotPaths(String dexPath,ClassLoader parent){
        Objects.requireNonNull(dexPath);List<byte[]> bytes=new ArrayList<>();List<Path> resources=new ArrayList<>();
        try{
            for(String name:dexPath.split(java.util.regex.Pattern.quote(File.pathSeparator),-1)){
                if(name.isEmpty())continue;
                Path requested=Path.of(name);
                Path file=requested.toRealPath();if(!Files.isRegularFile(file))throw new IOException("Dynamic DEX path is not a file");
                byte[] magic;try(InputStream stream=Files.newInputStream(file)){magic=stream.readNBytes(4);}
                if(Arrays.equals(magic,new byte[]{'d','e','x','\n'})){
                    long size=Files.size(file);
                    try(InputStream stream=Files.newInputStream(file)){byte[] data=stream.readAllBytes();if(data.length!=size)throw new IOException("Dynamic DEX changed while reading");bytes.add(data);}
                }else{
                    try(ZipFile zip=new ZipFile(file.toFile())){
                        List<? extends ZipEntry> entries=zip.stream().filter(e->e.getName().matches("classes(?:[1-9][0-9]*)?\\.dex")&&!e.getName().equals("classes1.dex")).sorted(Comparator.comparingLong(e->e.getName().equals("classes.dex")?1:dexIndex(e.getName()))).toList();
                        Set<String> names=new HashSet<>();
                        for(ZipEntry entry:entries){
                            if(!names.add(entry.getName()))throw new IOException("Ambiguous dynamic DEX entries");
                            long size=entry.getSize();if(size<0)throw new IOException("Invalid dynamic DEX entry length");
                            try(InputStream stream=zip.getInputStream(entry)){byte[] data=stream.readAllBytes();CRC32 crc=new CRC32();crc.update(data);if(data.length!=size||crc.getValue()!=entry.getCrc())throw new IOException("Dynamic DEX archive checksum mismatch");bytes.add(data);}
                        }
                    }
                    resources.add(file);
                }
            }
            return convert(bytes,parent,resources);
        }catch(IOException error){throw new IllegalArgumentException("Cannot open dynamic Android DEX",error);}
    }
    private static long dexIndex(String name){try{return Long.parseLong(name.substring(7,name.length()-4));}catch(NumberFormatException invalid){throw new IllegalArgumentException("Invalid multidex entry number",invalid);}}
    private static Module convert(List<byte[]> bytes,ClassLoader parent,List<Path> resources){
        Session session=current;if(session==null)throw new IllegalStateException("Dynamic DEX conversion is not configured");
        try{return session.open(bytes,parent,resources);}catch(InterruptedException error){Thread.currentThread().interrupt();throw new IllegalStateException("Dynamic DEX conversion was interrupted",error);}catch(IOException error){throw new IllegalArgumentException("Dynamic DEX conversion failed: "+error.getMessage(),error);}
    }
    public static final class Module implements Runnable,AutoCloseable {
        private final Session session;private final Artifact artifact;private final Path classArchive;private final JarFile jar;private final List<Path> resources;private final Set<String> members;private final Demand demand;private volatile boolean released;
        Module(Session session,Artifact artifact,List<Path> resources)throws IOException{this(session,artifact,null,resources);}
        Module(Session session,Artifact artifact,Demand demand,List<Path> resources)throws IOException{this.session=session;this.artifact=artifact;this.resources=List.copyOf(resources);members=demand==null?null:demand.members;classArchive=artifact==null?null:artifact.path;jar=classArchive==null?null:new JarFile(classArchive.toFile(),true);this.demand=demand;}
        private Module(Path prepared)throws IOException{this(prepared,(Set<String>)null);}
        private Module(Path prepared,Set<String> members)throws IOException{
            session=null;artifact=null;resources=List.of();classArchive=prepared;jar=prepared==null?null:new JarFile(prepared.toFile(),true);demand=null;
            try{this.members=members==null?indexedMembers(jar):Set.copyOf(members);}catch(IOException|RuntimeException|Error failure){if(jar!=null)jar.close();throw failure;}
        }
        private Module(Path index,List<Path> sources,ClassLoader parent,Set<String> selected)throws IOException{
            session=null;artifact=null;resources=List.of();classArchive=index;jar=new JarFile(index.toFile(),true);
            try{var all=indexedMembers(jar);if(all==null)throw new IOException("Android demand DEX index is missing");members=selected==null?all:Set.copyOf(selected);demand=new Demand(index,List.copyOf(sources),parent);demand.members=all;}
            catch(IOException|RuntimeException|Error failure){jar.close();throw failure;}
        }
        public void own(Object loader){CLEANER.register(loader,this);}
        public byte[] classBytes(String name)throws IOException{
            String internal=name.replace('.','/');
            synchronized(this){
                if(released)throw new IOException("Android DEX file is closed");
                if(jar==null||members!=null&&!members.contains(internal))return null;var entry=jar.getJarEntry(internal+".class");
                if(entry!=null){long size=entry.getSize();if(size<8)throw new IOException("Invalid dynamic class size");
                    try(InputStream stream=jar.getInputStream(entry)){byte[] data=stream.readNBytes((int)size+1);CRC32 crc=new CRC32();crc.update(data);if(data.length!=size||crc.getValue()!=entry.getCrc())throw new IOException("Dynamic class checksum mismatch");return data;}}
            }
            // Only the resident converter's byte protocol serializes cold misses.
            // Warm lookups and unrelated class definitions never own this module
            // monitor while waiting for conversion, cache publication or parents.
            return demand==null?null:demand.bytes(internal);
        }
        public List<Path> headers(){return classArchive==null?List.of():List.of(classArchive);}
        public synchronized String[] classNames(){if(released)throw new IllegalStateException("Android DEX file is closed");return jar==null?new String[0]:members!=null?members.stream().map(name->name.replace('/','.')).toArray(String[]::new):jar.stream().map(ZipEntry::getName).filter(name->name.endsWith(".class")).map(name->name.substring(0,name.length()-6).replace('/','.')).toArray(String[]::new);}
        public List<URL> resources(String name){
            List<URL> found=new ArrayList<>();if(name.indexOf('\0')>=0)return found;
            for(Path file:resources)try(ZipFile zip=new ZipFile(file.toFile())){if(zip.getEntry(name)!=null)found.add(URI.create("jar:"+file.toUri().toASCIIString()+"!/"+new URI(null,null,name,null).toASCIIString()).toURL());}catch(IOException|URISyntaxException error){throw new IllegalStateException("Dynamic module resource is unavailable",error);}
            return found;
        }
        @Override public synchronized void run(){if(released)return;released=true;try{if(jar!=null)jar.close();}catch(IOException ignored){}if(demand!=null)demand.close();if(artifact!=null)session.release(artifact);}
        @Override public void close(){run();}
    }
    private static Set<String> indexedMembers(JarFile jar)throws IOException{
        if(jar==null||jar.getJarEntry("META-INF/maclauncher/demand-dex")==null)return null;
        try(var marker=new DataInputStream(jar.getInputStream(jar.getJarEntry("META-INF/maclauncher/demand-dex")))){
            if(marker.readInt()!=1||marker.readNBytes(32).length!=32||marker.read()!=-1)throw new IOException("Android demand DEX index is invalid");
        }
        var entry=jar.getJarEntry("META-INF/maclauncher/apk-classes");if(entry==null)throw new IOException("Android DEX membership is missing");
        var members=new LinkedHashSet<String>();
        try(var input=new DataInputStream(jar.getInputStream(entry))){
            if(input.readInt()!=1)throw new IOException("Android DEX membership version is invalid");
            int sources=input.readInt();if(sources<0)throw new IOException("Android DEX source count is invalid");
            for(int i=0;i<sources;i++){input.readUTF();input.readBoolean();int count=input.readInt();if(count<0)throw new IOException("Android DEX class count is invalid");for(int j=0;j<count;j++)members.add(input.readUTF());}
            if(input.read()!=-1)throw new IOException("Android DEX membership is invalid");
        }
        return Collections.unmodifiableSet(members);
    }

    /** The existing Session remains the sole process, immutable byte-cache and
     * eviction owner. Each original DEX module retains one upstream parser. */
    private static final class Demand {
        final Path index;final List<Path> sources;final ClassLoader parent;
        List<Path> headers;List<byte[]> inputs;Set<String> members;
        volatile Session owner;String key;Path work;volatile Process child;volatile DataInputStream replies;volatile DataOutputStream requests;Thread diagnostics;
        final Object conversion=new Object();volatile boolean closed;
        final Map<String,byte[]> parentHeaders=new TreeMap<>();
        HeaderFacts parentFacts;
        final ByteArrayOutputStream errors=new ByteArrayOutputStream();
        Demand(Path index,List<Path> sources,ClassLoader parent){this.index=index;this.sources=sources;this.parent=parent;}
        Demand(Session owner,String key,ClassLoader parent,List<Path> headers,List<byte[]> inputs,Set<String> members){
            index=null;sources=List.of();this.owner=owner;this.key=key;this.parent=parent;this.headers=headers;this.inputs=inputs;this.members=members;
        }
        byte[] bytes(String name)throws IOException{
            if(owner==null){Session ready=current;if(ready==null)throw new IOException("Android DEX conversion is not configured");
                var resolved=ready.headers(parent);var digest=sha();for(Path file:ready.tools)ready.hashInput(digest,file);for(Path file:resolved)ready.hashInput(digest,file);hashDemandIndex(digest,index);
                String identity=HexFormat.of().formatHex(digest.digest());
                synchronized(conversion){if(owner==null){if(closed)throw new IOException("Android DEX file is closed");ready.track(this);headers=resolved;key=identity;owner=ready;}}}
            if(closed||owner.closed)throw new IOException("Dynamic DEX host has closed");
            String inputKey=classKey(key,name);
            Artifact artifact=owner.cached(inputKey,parent);if(artifact==null)artifact=owner.restoreClass(inputKey,parent,name);
            try{
                if(artifact==null){
                    synchronized(conversion){
                    if(closed||owner.closed)throw new IOException("Dynamic DEX host has closed");
                    // Another cold request may have completed while this caller
                    // waited for the single converter protocol. Warm requests
                    // never wait for that protocol or its parent lookups.
                    artifact=owner.cached(inputKey,parent);if(artifact==null)artifact=owner.restoreClass(inputKey,parent,name);
                    if(artifact!=null)return artifact.className.equals(name)?artifact.classData.clone():null;
                    // A class already resolved by a parent cannot change, but
                    // an original Android loader can gain another DEX element.
                    // Restart only when previously observed header facts change.
                    if(child!=null&&parent!=maclauncher.android.framework.HostDexFiles.bootClassLoader(null)){
                        boolean changed=false;for(var fact:parentHeaders.entrySet())if(!Arrays.equals(fact.getValue(),owner.conversionHeader(parent,fact.getKey()).hash)){changed=true;break;}
                        if(changed){stop();parentHeaders.clear();parentFacts=null;errors.reset();}
                    }
                    start();var request=requests;var reply=replies;
                    if(owner.closed||request==null||reply==null)throw new IOException("Dynamic DEX host has closed");
                    request.writeUTF(name);request.flush();
                    int operation;while((operation=reply.readUnsignedByte())==1){
                        String requested=reply.readUTF();ResolvedHeader header=owner.conversionHeader(parent,requested);
                        if(!Arrays.equals(parentHeaders.put(requested,header.hash),header.hash))parentFacts=null;
                        request.writeInt(header.bytes.length);request.write(header.bytes);request.flush();
                    }
                    if(operation!=0)throw new IOException("Android class converter response is invalid");
                    int length=reply.readInt();if(length<0)throw new IOException("Android converter returned an invalid class length");
                    byte[] data=reply.readNBytes(length);if(data.length!=length)throw new EOFException("Android class conversion was truncated");
                    String failure=reply.readUTF();if(!failure.isEmpty())throw new IOException("Android class could not be converted: "+name+": "+failure);
                    if(length==0)return null;
                    if(parentFacts==null)parentFacts=HeaderFacts.copy(parentHeaders);
                    String resolved=Session.resolvedKey(inputKey,parentFacts);
                    try{artifact=owner.publishClass(resolved,inputKey,parentFacts,name,data);owner.remember(resolved,artifact);}
                    catch(IOException unavailable){/* Optional cache retirement cannot reject usable bytes. */}
                    return data;
                    }
                }
                if(artifact.classData!=null)return artifact.className.equals(name)?artifact.classData.clone():null;
                try(var cached=new Module(owner,artifact,List.of())){artifact=null;return cached.classBytes(name);}
            }catch(EOFException failure){stop();throw new IOException("Android class converter stopped: "+errors.toString(java.nio.charset.StandardCharsets.UTF_8),failure);}
            finally{if(artifact!=null)owner.release(artifact);}
        }
        void prepareWork()throws IOException{
            if(owner.closed)throw new IOException("Dynamic DEX host has closed");
            if(work!=null)return;Path directory=Files.createTempDirectory(owner.root,"demand-");work=directory;
            try{
                Files.write(directory.resolve("headers.txt"),headers.stream().map(Path::toString).toList(),StandardOpenOption.CREATE_NEW);
                if(!sources.isEmpty())Files.write(directory.resolve("sources.txt"),sources.stream().map(Path::toString).toList(),StandardOpenOption.CREATE_NEW);
                var bytes=inputs;inputs=null;if(bytes!=null)for(int i=0;i<bytes.size();i++)Files.write(directory.resolve("input-"+i+".dex"),bytes.get(i),StandardOpenOption.CREATE_NEW);
            }catch(IOException failure){close();throw failure;}
        }
        void start()throws IOException{
            if(closed)throw new IOException("Android DEX file is closed");
            if(child!=null){if(!child.isAlive())throw new IOException("Android class converter stopped: "+errors.toString(java.nio.charset.StandardCharsets.UTF_8));return;}
            prepareWork();
            try{
                Path directory=work;if(owner.closed||directory==null)throw new IOException("Dynamic DEX host has closed");
                String target=index==null?"maclauncher.android.bridge.DynamicGuestNativeLoader":"maclauncher.android.bridge.GuestNativeLoader";
                var command=List.of(owner.javaExecutable.toString(),"-Xmx1024m","-Dmaclauncher.dex.nativeLoadTarget="+target,"-cp",String.join(File.pathSeparator,owner.tools.stream().map(Path::toString).toList()),"io.github.kksimp.dex2jvm.DemandDexConvert",directory.toString());
                child=owner.start(command);replies=new DataInputStream(new BufferedInputStream(child.getInputStream()));requests=new DataOutputStream(new BufferedOutputStream(child.getOutputStream()));diagnostics=drain(child,errors);
                if(closed)throw new IOException("Android DEX file is closed");
                if(replies.readInt()!=1)throw new IOException("Android class converter protocol is invalid");
                int count=replies.readInt();if(count<0)throw new IOException("Android class converter count is invalid");
                var names=new LinkedHashSet<String>();for(int i=0;i<count;i++)names.add(replies.readUTF());
                if(members!=null&&!members.equals(names))throw new IOException("Android DEX contents changed after preparation");members=Collections.unmodifiableSet(names);
            }catch(IOException|RuntimeException|Error failure){stop();throw failure;}
        }
        void stop(){
            Process process=child;child=null;
            try{if(requests!=null)requests.close();}catch(IOException ignored){}
            try{if(process!=null){if(!process.waitFor(1,TimeUnit.SECONDS)){process.destroyForcibly();process.waitFor(5,TimeUnit.SECONDS);}if(diagnostics!=null)diagnostics.join(1000);}}
            catch(InterruptedException interrupted){Thread.currentThread().interrupt();if(process!=null)process.destroyForcibly();}
            finally{
                try{if(replies!=null)replies.close();}catch(IOException ignored){}
                requests=null;replies=null;diagnostics=null;if(owner!=null&&process!=null)owner.forget(process);
            }
        }
        void close(){closed=true;stop();if(owner!=null)owner.untrack(this);if(work!=null)try{removeTree(work);}catch(IOException ignored){}work=null;inputs=null;}
    }
    private static String classKey(String inputKey,String name){var digest=sha();digest.update(inputKey.getBytes(java.nio.charset.StandardCharsets.UTF_8));digest.update(name.getBytes(java.nio.charset.StandardCharsets.UTF_8));return HexFormat.of().formatHex(digest.digest());}
    private static void hashDemandIndex(MessageDigest digest,Path index)throws IOException{
        // Preparation records the original ordered DEX digest. Repacking the
        // same index after a runtime update must not change those input bytes.
        try(var jar=new JarFile(index.toFile())){
            var marker=jar.getJarEntry("META-INF/maclauncher/demand-dex");
            if(marker==null)throw new IOException("Android demand DEX index is missing");
            try(var input=new DataInputStream(jar.getInputStream(marker))){
                int version=input.readInt();byte[] hash=input.readNBytes(32);
                if(version!=1||hash.length!=32||input.read()!=-1)throw new IOException("Android demand DEX index is invalid");
                digest.update("ordered-dex-input-v1".getBytes(java.nio.charset.StandardCharsets.UTF_8));digest.update(hash);
            }
        }
    }
    private static Thread drain(Process child,ByteArrayOutputStream diagnostics){
        Thread thread=new Thread(()->{try(InputStream stream=child.getErrorStream()){byte[] chunk=new byte[4096];for(int n;(n=stream.read(chunk))>=0;){int available=65536-diagnostics.size();if(available>0)diagnostics.write(chunk,0,Math.min(n,available));}}catch(IOException ignored){}},"Android DEX diagnostics");thread.setDaemon(true);thread.start();return thread;
    }
    /** Immutable conversion context shared by classes until the resident
     * parser observes a new parent fact. Its digest and accounting are computed
     * once, rather than copying and sorting the cumulative map per class. */
    private static final class HeaderFacts extends AbstractMap<String,byte[]> {
        final Map<String,byte[]> values;final byte[] hash;final long weight,persistentWeight;
        volatile boolean platformVerified;
        private HeaderFacts(Map<String,byte[]> source){
            values=Collections.unmodifiableMap(new TreeMap<>(source));var digest=sha();long bytes=128,persistent=36;
            for(var entry:values.entrySet()){
                byte[] name=entry.getKey().getBytes(java.nio.charset.StandardCharsets.UTF_8);
                digest.update(ByteBuffer.allocate(4).putInt(name.length).array());digest.update(name);digest.update(entry.getValue());
                bytes+=128L+entry.getKey().length()*2L;
                persistent+=34L+entry.getKey().length()*3L;
            }
            hash=digest.digest();weight=bytes;persistentWeight=persistent;
        }
        static HeaderFacts copy(Map<String,byte[]> source){return source instanceof HeaderFacts facts?facts:new HeaderFacts(source);}
        @Override public Set<Map.Entry<String,byte[]>> entrySet(){return values.entrySet();}
        @Override public int size(){return values.size();}
        boolean matches(Session owner,ClassLoader parent)throws IOException{
            boolean platform=parent==maclauncher.android.framework.HostDexFiles.bootClassLoader(null);
            // This exact loader has an immutable boot/framework class path.
            // Arbitrary wrappers can change delegation and are always queried.
            if(platform&&platformVerified)return true;
            for(var entry:values.entrySet())if(!Arrays.equals(entry.getValue(),owner.conversionHeader(parent,entry.getKey()).hash))return false;
            if(platform)platformVerified=true;return true;
        }
    }
    private static final class Artifact {
        final Path path;final long size,weight,persistentWeight;final String inputKey;final HeaderFacts parentHeaders;final AtomicInteger leases=new AtomicInteger();
        final String className;final byte[] classData;
        Artifact(Path path,long size,String inputKey,Map<String,byte[]> parentHeaders){this(path,size,inputKey,parentHeaders,null,null);}
        Artifact(Path path,long size,String inputKey,Map<String,byte[]> parentHeaders,String className,byte[] classData){this.path=path;this.size=size;this.inputKey=inputKey;this.parentHeaders=HeaderFacts.copy(parentHeaders);this.className=className;this.classData=classData;weight=size+128;persistentWeight=size+256+this.parentHeaders.persistentWeight;}
        boolean matches(Session owner,String key,ClassLoader parent)throws IOException{
            return inputKey.equals(key)&&parentHeaders.matches(owner,parent);
        }
    }
    private record ResolvedHeader(byte[] bytes,byte[] hash){}
    private static final ResolvedHeader ABSENT_HEADER=new ResolvedHeader(new byte[0],sha().digest(new byte[0]));
    // A Class's hierarchy cannot change after definition. ClassValue follows
    // that actual defining identity and retires with it, rather than retaining
    // a mutable delegating loader or guessing from its resources.
    private static final ClassValue<ResolvedHeader> RESOLVED_HEADERS=new ClassValue<>(){
        @Override protected ResolvedHeader computeValue(Class<?> type){
            try{
                var bytes=new ByteArrayOutputStream();
                try(var output=new DataOutputStream(bytes)){
                    output.writeInt(type.getModifiers());output.writeUTF(type.getName().replace('.','/'));
                    Class<?> superclass=type.getSuperclass();
                    output.writeUTF(superclass==null?(type.isInterface()?"java/lang/Object":""):superclass.getName().replace('.','/'));
                    Class<?>[] interfaces=type.getInterfaces();output.writeInt(interfaces.length);
                    for(Class<?> face:interfaces)output.writeUTF(face.getName().replace('.','/'));
                }
                byte[] encoded=bytes.toByteArray();return new ResolvedHeader(encoded,sha().digest(encoded));
            }catch(IOException impossible){throw new UncheckedIOException(impossible);}
        }
    };
    /** Header facts from the actual parent resolution, without class initialization.
     * Resource lookup alone is insufficient: a wrapper may delegate loadClass to
     * a loader which is neither its parent nor the owner of its resources. */
    private static ResolvedHeader parentHeader(ClassLoader parent,String name)throws IOException{
        try{
            // Class.forName can reuse a JVM initiating-loader association even
            // after a wrapper changes delegation. Query the loader's actual
            // current resolution; neither path initializes the resolved class.
            String binaryName=name.replace('/','.');
            Class<?> type=parent==null?Class.forName(binaryName,false,null):parent.loadClass(binaryName);
            return RESOLVED_HEADERS.get(type);
        }catch(ClassNotFoundException|LinkageError unavailable){return ABSENT_HEADER;}
    }
    private static void serveHeaders(Session owner,Process child,ClassLoader parent,Map<String,byte[]> hashes)throws IOException,InterruptedException{
        BlockingQueue<Object> pending=new LinkedBlockingQueue<>();Object end=new Object();
        // Pipe reads are not interruptible. Keep only those reads on a daemon;
        // the requesting thread resolves classes and can still cancel its wait.
        Thread reader=new Thread(()->{
            try(DataInputStream requests=new DataInputStream(child.getInputStream())){
                for(;;)pending.add(requests.readUTF());
            }catch(EOFException finished){pending.add(end);}
            catch(IOException failure){pending.add(failure);}
        },"Android DEX header requests");reader.setDaemon(true);reader.start();
        try(DataOutputStream replies=new DataOutputStream(child.getOutputStream())){
            for(;;){
                Object request=pending.take();if(request==end)return;if(request instanceof IOException failure)throw failure;
                String name=(String)request;
                ResolvedHeader header=owner.conversionHeader(parent,name);hashes.put(name,header.hash);
                replies.writeInt(header.bytes.length);replies.write(header.bytes);replies.flush();
            }
        }
    }
    private static final class Session {
        final Path root,shared,javaExecutable;final List<Path> tools,metadata;final Path application;final ClassLoader applicationLoader;
        private List<JarFile> metadataArchives;
        final ReentrantLock lock=new ReentrantLock(),publicationLock=new ReentrantLock();
        final Map<Path,byte[]> fixedHashes=new ConcurrentHashMap<>();
        final Map<String,ResolvedHeader> platformHeaders=new HashMap<>();long platformHeaderBytes;
        final LinkedHashMap<String,Artifact> artifacts=new LinkedHashMap<>(16,.75f,true);long cacheBytes;
        final IdentityHashMap<HeaderFacts,Integer> headerOwners=new IdentityHashMap<>();
        final Map<String,Set<Artifact>> byInputKey=new HashMap<>();
        final Set<Process> processes=ConcurrentHashMap.newKeySet();
        final Set<Demand> demands=ConcurrentHashMap.newKeySet();
        final Map<String,List<Path>> persistentEntries=new ConcurrentHashMap<>();
        final Set<Path> invalidEntries=ConcurrentHashMap.newKeySet();
        // A publication owns an ordinary artifact lease, without another byte
        // copy. Optional writer state has no ownership of the hot cache lock.
        final Set<String> queuedKeys=ConcurrentHashMap.newKeySet();
        final ConcurrentLinkedQueue<Publication> publications=new ConcurrentLinkedQueue<>();
        final ConcurrentLinkedQueue<Path> retiredFiles=new ConcurrentLinkedQueue<>();
        final Semaphore publicationReady=new Semaphore(0);
        final Object publicationState=new Object();
        Thread publisher;long publicationBytes;final AtomicBoolean maintenanceRequested=new AtomicBoolean(true);
        record Publication(String key,Artifact artifact){}
        long persistentAdded;
        final boolean demandAvailable;
        volatile boolean closed;
        Session(Path runtime,Path cache,Path application,ClassLoader loader)throws IOException{
            this.application=application;applicationLoader=loader;
            javaExecutable=Path.of(System.getProperty("java.home"),"bin","java").toRealPath();
            tools=List.of(regular(runtime,"converter-overlay.jar"),regular(runtime,"dex2jvm.jar"));
            metadata=List.of(regular(runtime,"android-api-metadata.jar"));
            try(var archive=new JarFile(tools.get(0).toFile())){demandAvailable=Boolean.getBoolean("maclauncher.dex.demand")&&archive.getJarEntry("io/github/kksimp/dex2jvm/DemandDexConvert.class")!=null;}
            Path parent=cache.resolve("dynamic-dex");Files.createDirectories(parent);if(Files.isSymbolicLink(parent)||!parent.toRealPath().startsWith(cache))throw new IOException("Invalid dynamic DEX cache");
            // Android package processes share cacheDir. Session publication and
            // dead-session reclamation therefore have one cross-process owner.
            // Hold the kernel lease only for filesystem mutation, never while
            // converting DEX or executing guest classes. A crashed JVM releases it.
            try(var channel=FileChannel.open(parent.resolve(".sessions.lock"),StandardOpenOption.CREATE,StandardOpenOption.WRITE,LinkOption.NOFOLLOW_LINKS);
                var lease=channel.lock()){
                shared=Files.createDirectories(parent.resolve("artifacts"));
                if(Files.isSymbolicLink(shared))throw new IOException("Invalid dynamic DEX artifact cache");
                root=Files.createTempDirectory(parent,"session-");
                try{
                    Files.setPosixFilePermissions(root,java.nio.file.attribute.PosixFilePermissions.fromString("rwx------"));
                    Files.writeString(root.resolve("owner"),ProcessHandle.current().pid()+"\n"+ProcessHandle.current().info().startInstant().orElse(Instant.EPOCH)+"\n",StandardOpenOption.CREATE_NEW);
                }catch(IOException failure){try{removeTree(root);}catch(IOException cleanup){failure.addSuppressed(cleanup);}throw failure;}
                // One immutable-entry inventory during process bootstrap, never
                // another complete directory scan for each requested class.
                try(var entries=Files.list(shared)){for(Path entry:entries.toList()){
                    String name=entry.getFileName().toString();if(PERSISTENT_ENTRY.matcher(name).matches())indexPersistent(name.substring(0,64),entry);
                }}catch(IOException|SecurityException unavailable){/* Conversion remains usable without persistent hits. */}
            }
            wakePublisher();
        }
        static Path regular(Path root,String name)throws IOException{Path file=root.resolve(name).toRealPath();if(!file.startsWith(root)||!Files.isRegularFile(file))throw new IOException("Dynamic converter file is unavailable");return file;}
        List<Path> headers(ClassLoader parent)throws IOException{
            LinkedHashSet<Path> result=new LinkedHashSet<>(metadata);
            for(ClassLoader at=parent;at!=null&&at!=Host.class.getClassLoader()&&at!=maclauncher.android.framework.HostDexFiles.bootClassLoader(null);at=at.getParent()){
                if(at==applicationLoader)result.add(application);
                else if(at instanceof dalvik.system.BaseDexClassLoader dex)result.addAll(maclauncher.android.framework.HostDexFiles.headers(dex));
                else if(at instanceof URLClassLoader urls){for(URL url:urls.getURLs())if(url.getProtocol().equals("file"))try{Path file=Path.of(url.toURI()).toRealPath();if(Files.isRegularFile(file))result.add(file);}catch(URISyntaxException error){throw new IOException(error);}}
                // Other parents expose their resolved hierarchy on demand through
                // the converter's resource API; no loader implementation list is needed.
            }
            return List.copyOf(result);
        }
        /** The parent header is exactly what the converter receives. Only an
         * unresolved parent falls back to the ordered complete metadata archive;
         * record that actual resource, rather than every unrelated declaration. */
        ResolvedHeader conversionHeader(ClassLoader parent,String name)throws IOException{
            boolean platform=parent==maclauncher.android.framework.HostDexFiles.bootClassLoader(null);
            // The exact boot loader has an immutable class path. Positive
            // resolutions retain their original defining Class's hierarchy;
            // arbitrary delegating loaders and absent/metadata facts still
            // resolve on every request. Never hold this lock while loading.
            if(platform)synchronized(platformHeaders){var cached=platformHeaders.get(name);if(cached!=null)return cached;}
            ResolvedHeader resolved=parentHeader(parent,name);
            if(resolved.bytes.length!=0){
                if(platform)synchronized(platformHeaders){
                    long weight=128L+name.length()*2L+resolved.bytes.length+resolved.hash.length;
                    if(!closed&&!platformHeaders.containsKey(name)&&weight<=PLATFORM_HEADER_TARGET-platformHeaderBytes){platformHeaders.put(name,resolved);platformHeaderBytes+=weight;}
                }
                return resolved;
            }
            for(var archive:metadataArchives()){
                var entry=archive.getJarEntry(name+".class");if(entry==null)continue;
                var digest=sha();digest.update("metadata-class-resource-v1".getBytes(java.nio.charset.StandardCharsets.UTF_8));
                try(var input=archive.getInputStream(entry)){byte[] chunk=new byte[4096];for(int n;(n=input.read(chunk))>=0;)digest.update(chunk,0,n);}
                return new ResolvedHeader(resolved.bytes,digest.digest());
            }
            return resolved;
        }
        private synchronized List<JarFile> metadataArchives()throws IOException{
            if(closed)throw new IOException("Dynamic DEX host has closed");
            if(metadataArchives==null){
                var archives=new ArrayList<JarFile>();
                try{for(Path file:metadata)archives.add(new JarFile(file.toFile()));metadataArchives=List.copyOf(archives);}
                catch(IOException failure){for(var archive:archives)try{archive.close();}catch(IOException cleanup){failure.addSuppressed(cleanup);}throw failure;}
            }
            return metadataArchives;
        }
        Process start(List<String> command)throws IOException{
            ProcessBuilder builder=new ProcessBuilder(command);
            builder.environment().keySet().removeIf(k->k.startsWith("DYLD_")||k.startsWith("DEX2JVM_")||Set.of("JAVA_TOOL_OPTIONS","JDK_JAVA_OPTIONS","_JAVA_OPTIONS","CLASSPATH").contains(k));
            if(closed)throw new IOException("Dynamic DEX host has closed");
            // Process creation can block in the OS. It never owns the byte-cache
            // lock. The post-registration check closes the shutdown/start race.
            Process child=builder.start();processes.add(child);
            if(closed){child.destroyForcibly();processes.remove(child);throw new IOException("Dynamic DEX host has closed");}
            return child;
        }
        void forget(Process child){processes.remove(child);}
        void track(Demand demand)throws IOException{if(closed)throw new IOException("Dynamic DEX host has closed");demands.add(demand);if(closed){demands.remove(demand);throw new IOException("Dynamic DEX host has closed");}}
        void untrack(Demand demand){demands.remove(demand);}
        Module demand(String key,List<byte[]> inputs,ClassLoader parent,List<Path> headers,List<Path> resources)throws IOException{
            String indexKey=classKey(key,"/DEX-index-v1");Artifact artifact=cached(indexKey,parent);if(artifact==null)artifact=restore(indexKey,parent);
            Demand demand=new Demand(this,key,parent,headers,inputs,null);Path pending=null;
            try{
                track(demand);
                if(artifact!=null){
                    try(var archive=new JarFile(artifact.path.toFile());var data=new DataInputStream(archive.getInputStream(archive.getJarEntry("META-INF/maclauncher/dynamic-classes")))){
                        int count=data.readInt();if(count<0)throw new IOException("Android DEX membership is invalid");var names=new LinkedHashSet<String>();for(int i=0;i<count;i++)names.add(data.readUTF());if(data.read()!=-1)throw new IOException("Android DEX membership is invalid");demand.members=Collections.unmodifiableSet(names);
                    }
                    // Retain the immutable input without retaining another copy
                    // of the complete SDK DEX in the game JVM's heap.
                    demand.prepareWork();
                }else{
                    demand.start();pending=Files.createTempFile(root,"index-",".jar");
                    try(var zip=new ZipOutputStream(new BufferedOutputStream(Files.newOutputStream(pending)));var data=new DataOutputStream(zip)){
                        var entry=new ZipEntry("META-INF/maclauncher/dynamic-classes");entry.setTime(315532800000L);zip.putNextEntry(entry);data.writeInt(demand.members.size());for(String name:demand.members)data.writeUTF(name);zip.closeEntry();
                    }
                    String resolved=resolvedKey(indexKey,Map.of());artifact=publish(resolved,new Artifact(pending,Files.size(pending),indexKey,Map.of()));remember(resolved,artifact);
                }
                Module module=new Module(this,artifact,demand,resources);artifact=null;demand=null;return module;
            }finally{if(artifact!=null)release(artifact);if(demand!=null)demand.close();if(pending!=null)Files.deleteIfExists(pending);}
        }
        Module open(List<byte[]> inputs,ClassLoader parent,List<Path> resources)throws IOException,InterruptedException{
            long started=System.nanoTime();
            if(closed)throw new IOException("Dynamic DEX host has closed");
            if(inputs.isEmpty())return new Module(this,null,resources);
            // The caller may own a guest monitor or class-loader lock. Neither
            // conversion nor parent resolution can hold our shared cache lock,
            // or wait for another caller that may need one of those monitors.
            List<Path> headers=headers(parent);MessageDigest digest=sha();
            for(Path input:tools)hashInput(digest,input);for(Path input:headers)hashInput(digest,input);
            for(byte[] input:inputs){digest.update(ByteBuffer.allocate(8).putLong(input.length).array());digest.update(input);}
            String key=HexFormat.of().formatHex(digest.digest());Artifact artifact=cached(key,parent);
            if(artifact==null){artifact=restore(key,parent);if(artifact!=null)report("persistent",started,0,inputs,artifact);}
            try{
                if(artifact==null&&demandAvailable)return demand(key,inputs,parent,headers,resources);
                if(artifact==null){
                    System.err.println("[android-dex] conversion started dexFiles="+inputs.size()+" waitMs=0");
                    Path work=Files.createTempDirectory(root,"convert-");
                    try{
                        for(int i=0;i<inputs.size();i++)Files.write(work.resolve("input-"+i+".dex"),inputs.get(i),StandardOpenOption.CREATE_NEW);
                        Files.write(work.resolve("headers.txt"),headers.stream().map(Path::toString).toList(),StandardOpenOption.CREATE_NEW);
                        List<String> command=List.of(javaExecutable.toString(),"-Dmaclauncher.dex.nativeLoadTarget=maclauncher.android.bridge.DynamicGuestNativeLoader","-cp",String.join(File.pathSeparator,tools.stream().map(Path::toString).toList()),"io.github.kksimp.dex2jvm.DynamicDexConvert",work.toString(),"--parent-headers");
                        Process child=start(command);
                        ByteArrayOutputStream diagnostics=new ByteArrayOutputStream();
                        Thread drain=drain(child,diagnostics);
                        Map<String,byte[]> parentHeaders=new TreeMap<>();
                        boolean exited=false;
                        // Serve on the requesting thread, which may already own a
                        // delegating loader's monitor. Resolving on a worker could deadlock.
                        try{serveHeaders(this,child,parent,parentHeaders);child.waitFor();exited=true;}
                        finally{try{if(!exited){child.destroyForcibly();child.waitFor(5,TimeUnit.SECONDS);}drain.join(5000);}
                            finally{forget(child);}}
                        if(closed)throw new IOException("Dynamic DEX host has closed");
                        if(child.exitValue()!=0)throw new IOException("Converter refused module: "+diagnostics.toString(java.nio.charset.StandardCharsets.UTF_8));
                        String report=diagnostics.toString(java.nio.charset.StandardCharsets.UTF_8);if(!report.isBlank())System.err.print(report);
                        Path output=work.resolve("classes.jar");long size=Files.size(output);
                        HeaderFacts facts=HeaderFacts.copy(parentHeaders);String resolvedKey=resolvedKey(key,facts);
                        artifact=publish(resolvedKey,new Artifact(output,size,key,facts));
                        remember(resolvedKey,artifact);
                        report("converted",started,0,inputs,artifact);
                    }finally{removeTree(work);}
                }
                if(closed)throw new IOException("Dynamic DEX host has closed");
                Module module=new Module(this,artifact,resources);artifact=null;return module;
            }finally{if(artifact!=null)release(artifact);}
        }
        Artifact cached(String key,ClassLoader parent)throws IOException{
            List<Artifact> candidates=new ArrayList<>();Artifact selected=null;
            lock.lock();try{
                if(closed)throw new IOException("Dynamic DEX host has closed");
                for(Artifact candidate:byInputKey.getOrDefault(key,Set.of())){candidate.leases.incrementAndGet();candidates.add(candidate);}
            }finally{lock.unlock();}
            try{
                for(Artifact candidate:candidates)if(candidate.matches(this,key,parent)){selected=candidate;break;}
                if(selected!=null){String resolved=resolvedKey(key,selected.parentHeaders);lock.lock();try{artifacts.get(resolved);}finally{lock.unlock();}}
                return selected;
            }finally{for(Artifact candidate:candidates)if(candidate!=selected)release(candidate);}
        }
        /** Publish once and acquire one lease; simultaneous equivalent conversions
         * can share bytes without sharing a defining loader or waiting on it. */
        Artifact publish(String key,Artifact candidate)throws IOException{
            // Immutable filesystem publication is prepared outside the hot
            // cache lock; duplicate callers only compare their final owner here.
            Path committed=root.resolve("archive-"+UUID.randomUUID()+".jar");
            Files.move(candidate.path,committed,StandardCopyOption.ATOMIC_MOVE);
            Artifact value=new Artifact(committed,candidate.size,candidate.inputKey,candidate.parentHeaders);
            boolean retained=false;
            lock.lock();try{
                if(closed)throw new IOException("Dynamic DEX host has closed");
                Artifact existing=artifacts.get(key);
                if(existing!=null){existing.leases.incrementAndGet();return existing;}
                evict(value);
                value.leases.set(1);artifacts.put(key,value);byInputKey.computeIfAbsent(value.inputKey,ignored->new LinkedHashSet<>()).add(value);retainHeaders(value);retained=true;return value;
            }finally{lock.unlock();if(!retained)retiredFiles.add(committed);if(!retiredFiles.isEmpty())wakePublisher();}
        }
        /** Demand classes enter the same bounded immutable cache without a
         * temporary archive or publication wait. */
        Artifact publishClass(String key,String inputKey,Map<String,byte[]> headers,String name,byte[] bytes)throws IOException{
            // Copy bytes/header facts before entering the cache's map mutation.
            Artifact candidate=new Artifact(null,bytes.length,inputKey,headers,name,bytes.clone());
            lock.lock();try{
                if(closed)throw new IOException("Dynamic DEX host has closed");
                Artifact value=artifacts.get(key);if(value!=null){value.leases.incrementAndGet();return value;}
                evict(candidate);candidate.leases.set(1);artifacts.put(key,candidate);byInputKey.computeIfAbsent(inputKey,ignored->new LinkedHashSet<>()).add(candidate);retainHeaders(candidate);return candidate;
            }finally{lock.unlock();if(!retiredFiles.isEmpty())wakePublisher();}
        }
        static String resolvedKey(String key,Map<String,byte[]> headers){
            MessageDigest resolved=sha();resolved.update(key.getBytes(java.nio.charset.StandardCharsets.UTF_8));resolved.update(HeaderFacts.copy(headers).hash);
            // Existing persistent entries retain the same input key and facts
            // format; restore validates them normally, without reconversion.
            return HexFormat.of().formatHex(resolved.digest());
        }
        static void report(String cache,long started,long waitingMs,List<byte[]> inputs,Artifact artifact){
            long inputBytes=0;for(byte[] input:inputs)inputBytes+=input.length;
            System.err.println("[android-dex] cache="+cache+" elapsedMs="+TimeUnit.NANOSECONDS.toMillis(System.nanoTime()-started)+" waitMs="+waitingMs+" inputBytes="+inputBytes+" outputBytes="+artifact.size);
        }
        private record CacheFacts(HeaderFacts headers,byte[] checksum){}
        void indexPersistent(String key,Path entry){persistentEntries.compute(key,(ignored,values)->{if(values==null)values=new CopyOnWriteArrayList<>();if(!values.contains(entry))values.add(entry);return values;});}
        void forgetPersistent(String key,Path entry){persistentEntries.computeIfPresent(key,(ignored,values)->{values.remove(entry);return values.isEmpty()?null:values;});}
        private static CacheFacts cacheFacts(Path entry)throws IOException{
            Path facts=entry.resolve("headers");if(Files.isSymbolicLink(entry)||!Files.isRegularFile(facts,LinkOption.NOFOLLOW_LINKS))throw new IOException("Incomplete DEX cache entry");
            Map<String,byte[]> hashes=new TreeMap<>();byte[] archiveHash;
            try(DataInputStream input=new DataInputStream(new BufferedInputStream(Files.newInputStream(facts)))){
                if(input.readInt()!=1)throw new IOException("Unknown DEX cache format");int count=input.readInt();
                if(count<0||count>100000)throw new IOException("Invalid DEX cache header count");
                for(int i=0;i<count;i++){String name=input.readUTF();byte[] hash=input.readNBytes(32);if(hash.length!=32)throw new EOFException();hashes.put(name,hash);}
                archiveHash=input.readNBytes(32);if(archiveHash.length!=32||input.read()!=-1)throw new IOException("Invalid DEX cache checksum");
            }
            return new CacheFacts(HeaderFacts.copy(hashes),archiveHash);
        }
        /** Class bytes use read-only immutable entries. A racing eviction is an
         * optional cache miss; neither publication nor pruning owns this reader. */
        Artifact restoreClass(String key,ClassLoader parent,String name)throws IOException{
            for(Path entry:persistentEntries.getOrDefault(key,List.of())){
                try{
                    CacheFacts facts=cacheFacts(entry);Path archive=entry.resolve("classes.jar");
                    if(!Files.isRegularFile(archive,LinkOption.NOFOLLOW_LINKS))throw new IOException("Incomplete DEX cache entry");
                    byte[] encoded=Files.readAllBytes(archive);MessageDigest checksum=sha();checksum.update(ByteBuffer.allocate(8).putLong(encoded.length).array());checksum.update(encoded);
                    if(!Arrays.equals(facts.checksum,checksum.digest()))throw new IOException("Changed DEX cache archive");
                    byte[] bytes;
                    try(var zip=new ZipInputStream(new ByteArrayInputStream(encoded))){
                        var member=zip.getNextEntry();if(member==null||!member.getName().equals(name+".class"))throw new IOException("Changed DEX class archive");
                        bytes=zip.readAllBytes();zip.closeEntry();if(zip.getNextEntry()!=null)throw new IOException("Invalid DEX class archive");
                    }
                    var candidate=new Artifact(null,bytes.length,key,facts.headers,name,bytes);
                    if(candidate.matches(this,key,parent))return publishClass(resolvedKey(key,facts.headers),key,facts.headers,name,bytes);
                }catch(IOException invalid){forgetPersistent(key,entry);invalidEntries.add(entry);}
            }return null;
        }
        /** Original eager/index archives keep a private hard link. Atomic link
         * acquisition and immutable contents preserve live readers when another
         * process evicts an entry; races simply miss, without a global wait. */
        Artifact restore(String key,ClassLoader parent)throws IOException{
            List<Artifact> candidates=new ArrayList<>();
            try{
                    for(Path entry:persistentEntries.getOrDefault(key,List.of())){
                        Path archive=entry.resolve("classes.jar");
                        Path local=root.resolve("restore-"+UUID.randomUUID()+".jar");
                        try{
                            CacheFacts facts=cacheFacts(entry);
                            if(!Files.isRegularFile(archive,LinkOption.NOFOLLOW_LINKS))throw new IOException("Incomplete DEX cache entry");
                            Files.createLink(local,archive);
                            MessageDigest checksum=sha();hashFile(checksum,local);if(!Arrays.equals(facts.checksum,checksum.digest()))throw new IOException("Changed DEX cache archive");
                            try(var checked=new JarFile(local.toFile(),true)){checked.size();}
                            candidates.add(new Artifact(local,Files.size(local),key,facts.headers));
                        }catch(IOException invalid){Files.deleteIfExists(local);forgetPersistent(key,entry);invalidEntries.add(entry);}
                    }
            }catch(IOException unavailable){for(Artifact candidate:candidates)Files.deleteIfExists(candidate.path);return null;}
            Artifact selected=null;
            try{
                for(Artifact candidate:candidates)if(selected==null&&candidate.matches(this,key,parent))selected=candidate;
                return selected==null?null:publish(resolvedKey(key,selected.parentHeaders),selected);
            }finally{for(Artifact candidate:candidates)try{Files.deleteIfExists(candidate.path);}catch(IOException ignored){}}
        }
        void remember(String resolvedKey,Artifact artifact){
            // Optional persistence must never hold a guest class-loader monitor
            // across filesystem writes, cache maintenance or another publisher.
            synchronized(publicationState){
                if(closed||artifact.persistentWeight>PERSISTENT_CACHE_TARGET-publicationBytes||!queuedKeys.add(resolvedKey))return;
                // The caller still owns a lease, so eviction cannot remove this
                // artifact between that lease and the writer's additional lease.
                publicationBytes+=artifact.persistentWeight;artifact.leases.incrementAndGet();publications.add(new Publication(resolvedKey,artifact));wakePublisher();
            }
        }
        void wakePublisher(){
            publicationReady.release();
            synchronized(publicationState){if(publisher==null&&!closed){publisher=new Thread(this::publishPending,"Android DEX cache writer");publisher.setDaemon(true);publisher.setPriority(Thread.MIN_PRIORITY);publisher.setContextClassLoader(Host.class.getClassLoader());publisher.start();}}
        }
        void publishPending(){
            for(;;){
                Path retired;while((retired=retiredFiles.poll())!=null)try{Files.deleteIfExists(retired);}catch(IOException ignored){}
                if(maintenanceRequested.getAndSet(false))prunePersistent();
                Publication next=publications.poll();
                if(next!=null){try{if(!closed)rememberNow(next.key,next.artifact);}finally{queuedKeys.remove(next.key);next.artifact.leases.decrementAndGet();synchronized(publicationState){publicationBytes-=next.artifact.persistentWeight;}}continue;}
                if(!closed)try{if(publicationReady.tryAcquire(1,TimeUnit.SECONDS)){publicationReady.drainPermits();continue;}}
                    catch(InterruptedException stop){Thread.currentThread().interrupt();}
                synchronized(publicationState){
                    if(closed){publisher=null;return;}
                    if(publications.isEmpty()&&retiredFiles.isEmpty()){publisher=null;return;}
                    publicationReady.drainPermits();
                }
            }
        }
        void rememberNow(String resolvedKey,Artifact artifact){
            // A cache write is optional: read-only/full disks must not reject a
            // successfully converted module. Publication never exposes half a pair.
            if(artifact.persistentWeight>PERSISTENT_CACHE_TARGET)return;
            Path pending=null,obsolete=null;boolean prune=false;
            try{
                Path destination=shared.resolve(artifact.inputKey+"-"+resolvedKey);
                // A sibling can publish after this process's immutable index
                // snapshot. Do not serialize the same bytes again merely to
                // discover that equivalent output already exists. This fast
                // path admits no cached code and needs no publication lease.
                if(!invalidEntries.contains(destination)&&Files.isDirectory(destination,LinkOption.NOFOLLOW_LINKS)){
                    indexPersistent(artifact.inputKey,destination);return;
                }
                pending=Files.createTempDirectory(root,"publish-");
                Path archive=pending.resolve("classes.jar");
                if(artifact.classData==null)Files.createLink(archive,artifact.path);
                else try(var zip=new ZipOutputStream(new BufferedOutputStream(Files.newOutputStream(archive,StandardOpenOption.CREATE_NEW),64*1024))){var entry=new ZipEntry(artifact.className+".class");entry.setTime(315532800000L);zip.putNextEntry(entry);zip.write(artifact.classData);zip.closeEntry();}
                try(DataOutputStream output=new DataOutputStream(new BufferedOutputStream(Files.newOutputStream(pending.resolve("headers"),StandardOpenOption.CREATE_NEW),64*1024))){
                    output.writeInt(1);output.writeInt(artifact.parentHeaders.size());
                    for(var entry:artifact.parentHeaders.entrySet()){output.writeUTF(entry.getKey());output.write(entry.getValue());}
                    MessageDigest checksum=sha();hashFile(checksum,archive);output.write(checksum.digest());
                }
                publicationLock.lock();
                try(var channel=FileChannel.open(shared.getParent().resolve(".sessions.lock"),StandardOpenOption.WRITE);var lease=channel.lock()){
                    if(invalidEntries.remove(destination)&&Files.exists(destination,LinkOption.NOFOLLOW_LINKS)){obsolete=shared.getParent().resolve(".retired-"+UUID.randomUUID());Files.move(destination,obsolete,StandardCopyOption.ATOMIC_MOVE);}
                    if(!Files.exists(destination,LinkOption.NOFOLLOW_LINKS)){
                        Files.move(pending,destination,StandardCopyOption.ATOMIC_MOVE);pending=null;
                        persistentAdded+=artifact.persistentWeight;
                        indexPersistent(artifact.inputKey,destination);
                    }
                    // Bound additional cache growth while amortizing metadata
                    // walks over many small demand-converted classes.
                    if(persistentAdded>=PERSISTENT_CACHE_TARGET/8){persistentAdded=0;prune=true;}
                }finally{publicationLock.unlock();}
            }catch(IOException|SecurityException unavailable){/* The process already owns the usable conversion. */}
            finally{if(pending!=null)try{removeTree(pending);}catch(IOException ignored){}if(obsolete!=null)try{removeTree(obsolete);}catch(IOException ignored){}}
            if(prune)prunePersistent();
        }
        /** Rename under the shared mutation lease; recursive removal happens
         * after releasing it, so starting a sibling process never waits on it. */
        Path retirePersistent(Path entry,boolean session)throws IOException{
            publicationLock.lock();
            try(var channel=FileChannel.open(shared.getParent().resolve(".sessions.lock"),StandardOpenOption.WRITE);var lease=channel.lock()){
                if(!Files.exists(entry,LinkOption.NOFOLLOW_LINKS)||session&&!deadSession(entry))return null;
                Path retired=shared.getParent().resolve(".retired-"+UUID.randomUUID());Files.move(entry,retired,StandardCopyOption.ATOMIC_MOVE);return retired;
            }finally{publicationLock.unlock();}
        }
        void prunePersistent(){
            try{
                try(var stream=Files.list(shared.getParent())){for(Path entry:stream.toList()){
                    String name=entry.getFileName().toString();if(Files.isSymbolicLink(entry)||!Files.isDirectory(entry,LinkOption.NOFOLLOW_LINKS))continue;
                    if(name.startsWith(".retired-")){try{removeTree(entry);}catch(IOException raced){}continue;}
                    if(SESSION_ENTRY.matcher(name).matches()){Path retired=retirePersistent(entry,true);if(retired!=null)try{removeTree(retired);}catch(IOException raced){}}
                }}
                record Entry(Path path,long modified,long size){}
                List<Entry> entries=new ArrayList<>();long total=0;
                try(var stream=Files.list(shared)){for(Path entry:stream.toList()){
                    if(!PERSISTENT_ENTRY.matcher(entry.getFileName().toString()).matches()||Files.isSymbolicLink(entry))continue;
                    try{long size=Files.size(entry.resolve("classes.jar"))+Files.size(entry.resolve("headers"));entries.add(new Entry(entry,Files.getLastModifiedTime(entry).toMillis(),size));total+=size;}
                    catch(IOException invalid){Path retired=retirePersistent(entry,false);if(retired!=null)try{removeTree(retired);}catch(IOException raced){}forgetPersistent(entry.getFileName().toString().substring(0,64),entry);invalidEntries.remove(entry);}
                }}
                // Snapshot each timestamp once. Neither sorting nor recursive
                // deletion belongs under the cross-process mutation lease.
                entries.sort(Comparator.comparingLong(Entry::modified));
                for(Entry entry:entries){if(total<=PERSISTENT_CACHE_TARGET)break;Path retired=retirePersistent(entry.path,false);if(retired!=null){total-=entry.size;try{removeTree(retired);}catch(IOException raced){}}
                    forgetPersistent(entry.path.getFileName().toString().substring(0,64),entry.path);invalidEntries.remove(entry.path);}
            }catch(IOException|SecurityException unavailable){/* Maintenance is optional; live bytes keep their original owners. */}
        }
        void hashInput(MessageDigest target,Path file)throws IOException{
            if(metadata.contains(file)){
                // Full declarations stay available to the converter. Their
                // consulted resource hashes live in HeaderFacts, so unrelated
                // framework members and ZIP metadata do not evict app code.
                target.update("consulted-metadata-resources-v1".getBytes(java.nio.charset.StandardCharsets.UTF_8));return;
            }
            // Included tools, headers and the prepared application are immutable
            // for this process. Do not reread them for every repeated SDK lookup.
            if(tools.contains(file)||metadata.contains(file)||file.equals(application)){
                try{target.update(fixedHashes.computeIfAbsent(file,source->{try{MessageDigest hash=sha();hashFile(hash,source);return hash.digest();}catch(IOException failure){throw new UncheckedIOException(failure);}}));}
                catch(UncheckedIOException failure){throw failure.getCause();}
            }else{MessageDigest hash=sha();hashFile(hash,file);target.update(hash.digest());}
        }
        void retainHeaders(Artifact artifact){cacheBytes+=artifact.weight;headerOwners.compute(artifact.parentHeaders,(facts,count)->{if(count==null)cacheBytes+=facts.weight;return count==null?1:count+1;});}
        void evict(Artifact requested){var it=artifacts.entrySet().iterator();while(it.hasNext()&&(cacheBytes+requested.weight+(headerOwners.containsKey(requested.parentHeaders)?0:requested.parentHeaders.weight)>CACHE_TARGET)){
            var entry=it.next();Artifact value=entry.getValue();if(value.leases.get()!=0)continue;if(value.path!=null)retiredFiles.add(value.path);cacheBytes-=value.weight;
            headerOwners.computeIfPresent(value.parentHeaders,(facts,count)->{if(count==1){cacheBytes-=facts.weight;return null;}return count-1;});
            it.remove();var indexed=byInputKey.get(value.inputKey);if(indexed!=null){indexed.remove(value);if(indexed.isEmpty())byInputKey.remove(value.inputKey);}
        }}
        void release(Artifact artifact){artifact.leases.decrementAndGet();}
        void shutdown(){
            List<Process> children;List<Demand> active;
            synchronized(publicationState){closed=true;children=List.copyOf(processes);active=List.copyOf(demands);}
            publicationReady.release();
            try{
                for(Process child:children)child.destroyForcibly();
                for(Process child:children)try{if(!child.waitFor(5,TimeUnit.SECONDS))throw new IllegalStateException("Dynamic DEX converter did not stop");}
                    catch(InterruptedException error){Thread.currentThread().interrupt();throw new IllegalStateException("Interrupted while stopping dynamic DEX conversion",error);}
                for(Demand demand:active)demand.close();
            }finally{
                synchronized(platformHeaders){platformHeaders.clear();platformHeaderBytes=0;}
                synchronized(this){if(metadataArchives!=null){for(var archive:metadataArchives)try{archive.close();}catch(IOException ignored){}metadataArchives=null;}}
            }
        }
    }
    private static MessageDigest sha(){return HostDigests.sha256();}
    private static void hashFile(MessageDigest digest,Path file)throws IOException{digest.update(ByteBuffer.allocate(8).putLong(Files.size(file)).array());try(InputStream stream=Files.newInputStream(file)){byte[] chunk=new byte[65536];for(int n;(n=stream.read(chunk))>=0;)digest.update(chunk,0,n);}}
    private static boolean deadSession(Path dir)throws IOException{
            Path owner=dir.resolve("owner");
            // Under the shared mutation lease, an unpublished owner can only
            // be a constructor interrupted by process death, not a live peer.
            if(!Files.exists(owner,LinkOption.NOFOLLOW_LINKS))return true;
            if(!Files.isRegularFile(owner,LinkOption.NOFOLLOW_LINKS)||Files.size(owner)>256)return false;
            try{List<String> lines=Files.readAllLines(owner);long pid=Long.parseLong(lines.get(0));Instant start=Instant.parse(lines.get(1));var process=ProcessHandle.of(pid);return process.isEmpty()||!process.get().isAlive()||!process.get().info().startInstant().orElse(start).equals(start);}catch(IllegalArgumentException|IndexOutOfBoundsException ignored){return false;}
    }
    private static void removeTree(Path root)throws IOException{try(var paths=Files.walk(root)){for(Path path:paths.sorted(Comparator.reverseOrder()).toList())Files.deleteIfExists(path);}}
}
