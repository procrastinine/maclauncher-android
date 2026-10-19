package io.github.kksimp.dex2jvm;

import com.android.apksig.ApkVerifier;
import com.android.apksig.util.DataSources;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;

/** Verifies untouched APK bytes, including signing blocks; never signs packages. */
public final class VerifyApkSet {
    public static void main(String[] args) throws Exception {
        if(args.length<2||args.length>65)throw new IllegalArgumentException("Expected 1–64 APKs");
        StringBuilder report=new StringBuilder();
        for(int i=1;i<args.length;i++){
            Path file=Path.of(args[i]);
            if(!Files.isRegularFile(file,LinkOption.NOFOLLOW_LINKS)||Files.size(file)>4L*1024*1024*1024)
                throw new IllegalArgumentException("Invalid Android package file");
            try(FileChannel channel=FileChannel.open(file,StandardOpenOption.READ,LinkOption.NOFOLLOW_LINKS)){
                var result=new ApkVerifier.Builder(DataSources.asDataSource(channel)).build().verify();
                if(!result.isVerified()||result.getSignerCertificates().isEmpty())
                    throw new IllegalArgumentException("Android package signature verification failed: "+file.getFileName()+" "+result.getErrors().stream().limit(4).toList());
                var signers=new TreeSet<String>();var digest=MessageDigest.getInstance("SHA-256");
                for(var certificate:result.getSignerCertificates())signers.add(HexFormat.of().formatHex(digest.digest(certificate.getEncoded())));
                ByteBuffer buffer=ByteBuffer.allocate(65536);channel.position(0);
                while(channel.read(buffer)!=-1){buffer.flip();digest.update(buffer);buffer.clear();}
                report.append(i-1).append('\t').append(HexFormat.of().formatHex(digest.digest())).append('\t')
                    .append(String.join(",",signers)).append('\n');
            }
        }
        Files.writeString(Path.of(args[0]),report,StandardOpenOption.CREATE_NEW);
    }
}
