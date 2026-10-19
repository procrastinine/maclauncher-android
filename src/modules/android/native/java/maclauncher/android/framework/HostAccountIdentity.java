package maclauncher.android.framework;

import android.content.pm.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import org.json.*;

/** Keeps persistent account storage with its actual APK signing identity. */
public final class HostAccountIdentity {
    private HostAccountIdentity(){}
    public static void bind(Path root,String packageName,SigningDetails current)throws Exception{
        if(current==SigningDetails.UNKNOWN||!current.hasSignatures())throw new SecurityException("The account provider has no verified signing identity");
        Path directory=root.resolve("system");Files.createDirectories(directory);Path file=directory.resolve("account-provider.json");
        if(Files.exists(file,LinkOption.NOFOLLOW_LINKS)){
            String text;
            try(var input=Files.newInputStream(file,LinkOption.NOFOLLOW_LINKS)){text=new String(input.readAllBytes(),StandardCharsets.UTF_8);}
            JSONObject previous=new JSONObject(text);
            if(previous.getInt("schemaVersion")!=1||!packageName.equals(previous.getString("packageName")))throw new SecurityException("Account storage belongs to another provider");
            JSONArray values=previous.getJSONArray("signatures");Signature[] signatures=new Signature[values.length()];for(int i=0;i<signatures.length;i++)signatures[i]=new Signature(values.getString(i));
            SigningDetails owner=new SigningDetails(signatures,previous.getInt("signatureScheme"));
            if(!current.checkCapability(owner,SigningDetails.CertCapabilities.INSTALLED_DATA))throw new SecurityException("The installed account provider cannot access this signing identity's data");
            if(current.signaturesMatchExactly(owner))return;
        }
        JSONArray signatures=new JSONArray();for(Signature signer:current.getSignatures())signatures.put(signer.toCharsString());
        String value=new JSONObject().put("schemaVersion",1).put("packageName",packageName).put("signatureScheme",current.getSignatureSchemeVersion()).put("signatures",signatures).toString()+"\n";
        Path temporary=Files.createTempFile(directory,"account-provider-",".json");
        try{try(var writer=Files.newBufferedWriter(temporary,StandardCharsets.UTF_8)){writer.write(value);}Files.move(temporary,file,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);}
        finally{Files.deleteIfExists(temporary);}
    }
}
