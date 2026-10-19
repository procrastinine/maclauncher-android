package maclauncher.android.framework;
import java.io.*;
/** JDK FileInputStream bulk reads seek; Android pipe streams need InputStream's sequential loop. */
public final class HostSequentialInput {
    private HostSequentialInput() {}
    private static InputStream sequential(InputStream input){return new InputStream(){public int read()throws IOException{return input.read();}public int read(byte[] data,int offset,int size)throws IOException{return input.read(data,offset,size);}};}
    public static byte[] readAllBytes(InputStream input)throws IOException{return sequential(input).readAllBytes();}
    public static byte[] readNBytes(InputStream input,int count)throws IOException{return sequential(input).readNBytes(count);}
    public static long transferTo(InputStream input,OutputStream output)throws IOException{return sequential(input).transferTo(output);}
}
