package maclauncher.android;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Versioned launcher-to-host data. Bounded paths and data; no arbitrary JVM arguments. */
public final class LaunchConfig {
    /** Complete UTF-8 file, including launcher trailing newline. Mirrors host.js. */
    public static final int MAX_BYTES=64*1024;
    private static final Set<String> ALLOWED=Set.of("schemaVersion","runtimeRoot","packageName","versionName","versionCode","activityClass","convertedJar","nativeRoot","assetsRoot","resourcesRoot","sourceApk","filesRoot","cacheRoot","title","iconFile","width","height","graphicsLibrary","sourceApks","processExitHistoryJson","webLinksAllowed","networkAllowed","allowAds","audioInputUid","cameraInputUid","authorityFd","processName","serviceProcess","installedPackagesJson","accountControlFd","googleProviderCompatibility","manualTilt","accountService","accountSocketPath","accountProviderPackage","serviceOperation","operationId","googleFreeAcquisition","touchJoystick","touchJoystickX","touchJoystickY","touchJoystickRadius","nativeInputFd","nativeControlFd");
    private final Map<String,Object> values;
    private LaunchConfig(Map<String,Object> values){this.values=Map.copyOf(values);}
    public static LaunchConfig read(Path file)throws IOException{
        byte[] bytes;try(var stream=Files.newInputStream(file)){bytes=stream.readNBytes(MAX_BYTES+1);}if(bytes.length<2||bytes.length>MAX_BYTES)throw new IOException("Android launch configuration size is invalid");
        String input=StandardCharsets.UTF_8.newDecoder().decode(java.nio.ByteBuffer.wrap(bytes)).toString();Parser parser=new Parser(input);Map<String,Object> values=parser.object();
        for(String key:values.keySet())if(!ALLOWED.contains(key))throw new IOException("Unknown Android launch option: "+key);
        LaunchConfig config=new LaunchConfig(values);if(config.number("schemaVersion")!=1)throw new IOException("Unsupported Android launch configuration version");
        if(config.number("width")<64||config.number("height")<64||config.number("width")>8192||config.number("height")>8192)throw new IOException("Android viewport dimensions must be between 64 and 8192");
        if(config.number("versionCode")<0||config.number("versionCode")>Integer.MAX_VALUE)throw new IOException("Android version code is outside the supported range");
        for(String key:config.serviceProcess()?List.of("packageName"):List.of("packageName","activityClass"))if(!config.text(key).matches("[A-Za-z_$][\\w$]*(?:\\.[A-Za-z_$][\\w$]*)+")&&!(key.equals("packageName")&&config.accountService()&&config.text(key).equals("android")))throw new IOException("Invalid Android "+key);
        if(config.serviceProcess()){config.text("processName");if(!values.containsKey("authorityFd"))throw new IOException("Android service process requires its supervisor connection");}
        if(values.containsKey("authorityFd"))config.descriptor("authorityFd");
        if(values.containsKey("nativeInputFd")||values.containsKey("nativeControlFd")){
            if(config.serviceProcess()||!values.containsKey("authorityFd"))throw new IOException("Native Android output requires its supervisor connection");
            int inputDescriptor=config.descriptor("nativeInputFd"),control=config.descriptor("nativeControlFd");if(inputDescriptor==control||inputDescriptor==config.descriptor("authorityFd")||control==config.descriptor("authorityFd"))throw new IOException("Native Android output requires separate descriptors");
        }
        if(values.containsKey("serviceOperation")||values.containsKey("operationId")){
            if(!config.serviceProcess()||config.accountService()||!config.text("serviceOperation").equals("play-library")||!config.text("operationId").matches("[a-f0-9-]{36}")||!config.networkAllowed()||!config.googleProviderCompatibility())throw new IOException("Invalid Android service operation");
        }
        if(values.containsKey("googleFreeAcquisition")&&(!config.serviceProcess()||!config.text("packageName").equals("com.google.android.gms")||config.number("googleFreeAcquisition")!=1||!config.networkAllowed()||!config.googleProviderCompatibility()))throw new IOException("Invalid Google library provider operation");
        if(values.containsKey("accountControlFd")){
            int account=config.descriptor("accountControlFd");
            if(!config.serviceProcess()||!values.containsKey("authorityFd")||account==config.descriptor("authorityFd"))throw new IOException("Android account control requires a separate supervised service connection");
        }
        for(String key:List.of("runtimeRoot","nativeRoot","assetsRoot","resourcesRoot","filesRoot","cacheRoot"))config.directory(key);
        for(String key:List.of("sourceApk","convertedJar"))config.file(key);
        if(values.containsKey("iconFile")&&!config.file("iconFile").startsWith(config.directory("cacheRoot")))throw new IOException("Android application icon must remain in private cache storage");
        config.touchJoystick();config.touchJoystickPosition("touchJoystickX",1800);config.touchJoystickPosition("touchJoystickY",7500);config.touchJoystickPosition("touchJoystickRadius",1200);
        config.manualTilt();config.webLinksAllowed();config.networkAllowed();config.allowAds();config.googleProviderCompatibility();config.inputUid("audioInputUid");config.inputUid("cameraInputUid");config.sourceApks();config.text("versionName");config.text("title");if(values.containsKey("graphicsLibrary"))config.file("graphicsLibrary");return config;
    }
    public byte[] icon()throws IOException{if(!values.containsKey("iconFile"))return null;try(var input=Files.newInputStream(file("iconFile"))){byte[] bytes=input.readNBytes(8*1024*1024+1);if(bytes.length>8*1024*1024)throw new IOException("Android application icon is too large");return bytes;}}
    public ApplicationSettings applicationSettings()throws IOException{return ApplicationSettings.read(values::get);}
    public String manualTilt()throws IOException{return applicationSettings().manualTilt();}
    public String touchJoystick()throws IOException{return applicationSettings().touchJoystick();}
    public float touchJoystickPosition(String name,int fallback)throws IOException{return ApplicationSettings.read(key->key.equals(name)&&!values.containsKey(key)?fallback:values.get(key)).position(name);}
    public boolean serviceProcess()throws IOException{if(!values.containsKey("serviceProcess"))return false;long value=number("serviceProcess");if(value!=1)throw new IOException("Invalid Android service process role");return true;}
    public boolean accountService()throws IOException{if(!values.containsKey("accountService"))return false;if(number("accountService")!=1||!serviceProcess()||!text("packageName").equals("android"))throw new IOException("Invalid Android account service role");return true;}
    public int descriptor(String name)throws IOException{long value=number(name);if(value<3||value>Integer.MAX_VALUE)throw new IOException("Invalid inherited Android descriptor");return (int)value;}
    public boolean contains(String name){return values.containsKey(name);}
    public boolean webLinksAllowed()throws IOException{if(!values.containsKey("webLinksAllowed"))return false;long value=number("webLinksAllowed");if(value!=0&&value!=1)throw new IOException("Invalid Android web-link policy");return value==1;}
    public boolean networkAllowed()throws IOException{if(!values.containsKey("networkAllowed"))return false;long value=number("networkAllowed");if(value!=0&&value!=1)throw new IOException("Invalid Android network policy");return value==1;}
    public boolean allowAds()throws IOException{if(!values.containsKey("allowAds"))return false;long value=number("allowAds");if(value!=0&&value!=1)throw new IOException("Invalid Android advertising policy");return value==1;}
    public boolean googleProviderCompatibility()throws IOException{if(!values.containsKey("googleProviderCompatibility"))return false;long value=number("googleProviderCompatibility");if(value!=0&&value!=1)throw new IOException("Invalid Google provider policy");return value==1;}
    public String inputUid(String name)throws IOException{
        return applicationSettings().input(name);
    }
    /** Optional inspected sibling APK list. This does not discover/install split packages. */
    public List<Path> sourceApks()throws IOException {
        Path base=file("sourceApk");Object raw=values.get("sourceApks");
        if(raw==null)return List.of(base);
        if(!(raw instanceof List<?> entries)||entries.isEmpty()||entries.size()>64)throw new IOException("Invalid Android source APK list");
        Path directory=base.getParent();List<Path> result=new ArrayList<>();Set<Object> identities=new HashSet<>();
        for(Object entry:entries){
            if(!(entry instanceof String name)||name.isEmpty()||name.length()>4096||name.indexOf('\0')>=0)throw new IOException("Invalid Android source APK path");
            Path requested;try{requested=Path.of(name);}catch(InvalidPathException error){throw new IOException("Invalid Android source APK path",error);}
            if(!requested.isAbsolute()){
                if(name.equals(".")||name.equals("..")||name.indexOf('/')>=0||name.indexOf('\\')>=0)throw new IOException("Android source APK names must be sibling filenames");
                requested=directory.resolve(requested);
            }
            if(Files.isSymbolicLink(requested))throw new IOException("Symbolic Android source APK files are unsupported");
            Path file=requested.toRealPath();
            if(!file.getParent().equals(directory)||!Files.isRegularFile(file))throw new IOException("Android source APK is not a regular sibling file");
            if(result.isEmpty()&&!file.equals(base))throw new IOException("Android source APK list must begin with the base APK");
            var attributes=Files.readAttributes(file,java.nio.file.attribute.BasicFileAttributes.class);
            Object identity=attributes.fileKey();
            if(result.contains(file)||(identity!=null&&!identities.add(identity)))throw new IOException("Duplicate Android source APK");
            for(Path prior:result)if(Files.isSameFile(prior,file))throw new IOException("Duplicate Android source APK identity");
            result.add(file);
        }
        return List.copyOf(result);
    }
    public String text(String name)throws IOException{Object value=values.get(name);if(!(value instanceof String text)||text.isEmpty()||text.length()>16384||text.indexOf('\0')>=0)throw new IOException("Invalid Android launch value: "+name);return text;}
    public String optional(String name)throws IOException{return values.containsKey(name)?text(name):null;}
    public long number(String name)throws IOException{Object value=values.get(name);if(!(value instanceof Long number))throw new IOException("Invalid Android launch number: "+name);return number;}
    private Path resolved(String name)throws IOException{Path value=Path.of(text(name));if(!value.isAbsolute())throw new IOException("Android launch paths must be absolute");return value.toRealPath();}
    public Path directory(String name)throws IOException{Path value=resolved(name);if(!Files.isDirectory(value))throw new IOException("Android launch directory is missing: "+name);return value;}
    public Path file(String name)throws IOException{Path value=resolved(name);if(!Files.isRegularFile(value))throw new IOException("Android launch file is missing: "+name);return value;}
    public Path runtimeFile(String name)throws IOException{Path root=directory("runtimeRoot"),file=root.resolve(name).toRealPath();if(!file.startsWith(root)||!Files.isRegularFile(file))throw new IOException("Android runtime file is missing: "+name);return file;}
    public Path runtimeDirectory(String name)throws IOException{Path root=directory("runtimeRoot"),file=root.resolve(name).toRealPath();if(!file.startsWith(root)||!Files.isDirectory(file))throw new IOException("Android runtime directory is missing: "+name);return file;}
    private static final class Parser {
        final String input;int position;
        Parser(String input){this.input=input;}
        void space(){while(position<input.length()&&" \r\n\t".indexOf(input.charAt(position))>=0)position++;}
        IOException invalid(){return new IOException("Invalid Android launch JSON at character "+position);}
        void take(char expected)throws IOException{space();if(position>=input.length()||input.charAt(position++)!=expected)throw invalid();}
        Map<String,Object> object()throws IOException{Map<String,Object> out=new LinkedHashMap<>();take('{');space();if(position<input.length()&&input.charAt(position)=='}'){position++;}else{while(true){String key=string();take(':');space();Object value;if(position<input.length()&&input.charAt(position)=='"')value=string();else if(position<input.length()&&input.charAt(position)=='['){if(!key.equals("sourceApks"))throw invalid();value=strings();}else value=number();if(out.putIfAbsent(key,value)!=null||out.size()>ALLOWED.size())throw invalid();space();if(position>=input.length())throw invalid();char next=input.charAt(position++);if(next=='}')break;if(next!=',')throw invalid();}}space();if(position!=input.length())throw invalid();return out;}
        List<String> strings()throws IOException{List<String> out=new ArrayList<>();take('[');space();if(position<input.length()&&input.charAt(position)==']'){position++;return List.of();}while(true){if(out.size()>=64)throw invalid();out.add(string());space();if(position>=input.length())throw invalid();char next=input.charAt(position++);if(next==']')return List.copyOf(out);if(next!=',')throw invalid();}}
        long number()throws IOException{space();int start=position;if(position<input.length()&&input.charAt(position)=='-')position++;int digits=position;while(position<input.length()&&input.charAt(position)>='0'&&input.charAt(position)<='9')position++;if(position==digits||position-digits>1&&input.charAt(digits)=='0')throw invalid();try{return Long.parseLong(input.substring(start,position));}catch(NumberFormatException error){throw invalid();}}
        String string()throws IOException{take('"');StringBuilder value=new StringBuilder();while(position<input.length()){char c=input.charAt(position++);if(c=='"'){for(int i=0;i<value.length();i++)if(Character.isSurrogate(value.charAt(i))){if(!Character.isHighSurrogate(value.charAt(i))||i+1>=value.length()||!Character.isLowSurrogate(value.charAt(++i)))throw invalid();}return value.toString();}if(c<' ')throw invalid();if(c=='\\'){if(position>=input.length())throw invalid();char escaped=input.charAt(position++);c=switch(escaped){case '"','\\','/'->escaped;case 'b'->'\b';case 'f'->'\f';case 'n'->'\n';case 'r'->'\r';case 't'->'\t';case 'u'->{if(position+4>input.length())throw invalid();int code=0;for(int i=0;i<4;i++){int digit=Character.digit(input.charAt(position++),16);if(digit<0)throw invalid();code=code*16+digit;}yield(char)code;}default->throw invalid();};}value.append(c);if(value.length()>49152)throw invalid();}throw invalid();}
    }
}
