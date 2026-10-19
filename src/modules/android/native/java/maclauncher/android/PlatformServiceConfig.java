package maclauncher.android;

import java.io.IOException;
import java.nio.file.*;
import org.json.*;

/** Host-authored profile process configuration; no APK or game-selected paths. */
record PlatformServiceConfig(String role,Path runtimeRoot,Path dataRoot,Path filesRoot,Path cacheRoot,
                             Path runRoot,int authorityFd,int ownershipFd,Path platformSocketPath) {
    String packageName(){return role.equals("system")?"android":"android.keystore";}
    String processName(){return role.equals("system")?"android.system":"android.keystore";}
    int uid(){return role.equals("system")?1000:1017;}
    String securityContext(){return role.equals("system")?"u:r:system_server:s0":"u:r:keystore:s0";}
    static PlatformServiceConfig read(Path path)throws IOException,JSONException {
        JSONObject value=new JSONObject(java.nio.charset.StandardCharsets.UTF_8.newDecoder()
            .decode(java.nio.ByteBuffer.wrap(Files.readAllBytes(path))).toString());
        String role=value.getString("serviceRole");
        if(!role.equals("keystore")&&!role.equals("system"))throw new IllegalArgumentException("Unknown Android platform service role");
        Path runtime=directory(value,"runtimeRoot"),data=directory(value,"dataRoot");
        Path files=directory(value,"filesRoot"),cache=directory(value,"cacheRoot"),run=directory(value,"runRoot");
        if(!files.startsWith(data)||files.equals(data)||!cache.startsWith(data)||cache.equals(data))throw new SecurityException("Android platform directories must belong to the profile service");
        Path socket=Path.of(value.getString("platformSocketPath")).toAbsolutePath().normalize();
        Path parent=socket.getParent().toRealPath();
        if(!parent.startsWith(run)||Files.exists(socket,LinkOption.NOFOLLOW_LINKS))throw new SecurityException("Android platform socket is not a new service-owned path");
        int authority=value.getInt("authorityFd"),ownership=value.getInt("ownershipFd");
        if(authority<0||ownership<0||authority==ownership)throw new IllegalArgumentException("Android platform authority descriptors are missing");
        return new PlatformServiceConfig(role,runtime,data,files,cache,run,authority,ownership,parent.resolve(socket.getFileName()));
    }
    private static Path directory(JSONObject value,String key)throws IOException,JSONException {
        Path result=Path.of(value.getString(key)).toRealPath();
        if(!Files.isDirectory(result))throw new IOException("Android platform directory is unavailable: "+key);
        return result;
    }
}
