package maclauncher.android.bridge;
import java.io.IOException;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Metadata strings use validated ELF table bounds; executable loading remains the real Klepton loader. */
final class ElfNativeExports {
    record Metadata(Set<String> exports,List<String> dependencies){}
    private record Layout(boolean wide,int sectionOffset,int sectionEntry,int sectionCount,int sectionSize,int dataOffset,int dataSize,int link,int entrySize,int symbolSize,int symbolInfo,int symbolSection,int symbolValue) {
        long word(ByteBuffer buffer,int offset){return wide?buffer.getLong(offset):Integer.toUnsignedLong(buffer.getInt(offset));}
        int wordSize(){return wide?8:4;}
    }
    private static final Layout ARM64=new Layout(true,40,58,60,64,24,32,40,56,24,4,6,8);
    private static final Layout ARM32=new Layout(false,32,46,48,40,16,20,24,36,16,12,14,4);
    static Set<String> read(Path file)throws IOException{return readMetadata(file).exports();}
    static Metadata readMetadata(Path file)throws IOException {
        long length=Files.size(file);
        if(length<52||length>256L*1024*1024)throw new IOException("Unsupported native library size");
        ByteBuffer b=ByteBuffer.wrap(Files.readAllBytes(file)).order(ByteOrder.LITTLE_ENDIAN);
        if(b.getInt(0)!=0x464c457f||b.get(5)!=1)throw new IOException("Android native library must be little-endian ELF");
        int machine=Short.toUnsignedInt(b.getShort(18));
        Layout layout=b.get(4)==2&&machine==183?ARM64:b.get(4)==1&&machine==40?ARM32:null;
        if(layout==null)throw new IOException("Android native library must use ARM64 or ARM32 instructions");
        range(b,0,layout.wide()?64:52);
        long offset=layout.word(b,layout.sectionOffset());int size=Short.toUnsignedInt(b.getShort(layout.sectionEntry())),count=Short.toUnsignedInt(b.getShort(layout.sectionCount()));
        if(size<layout.sectionSize()||count==0||count>16384)throw new IOException("Native symbol sections are unavailable");
        range(b,offset,(long)size*count);
        Set<String> exports=new HashSet<>();List<String> dependencies=new ArrayList<>();
        for(int i=0;i<count;i++){
            int header=(int)(offset+(long)i*size),type=b.getInt(header+4);if(type==6){
                long data=layout.word(b,header+layout.dataOffset()),bytes=layout.word(b,header+layout.dataSize()),entrySize=layout.word(b,header+layout.entrySize());int linked=b.getInt(header+layout.link());
                if(entrySize<layout.wordSize()*2||bytes%entrySize!=0||bytes/entrySize>65536||linked<0||linked>=count)throw new IOException("Invalid native dynamic table");
                range(b,data,bytes);int strings=(int)(offset+(long)linked*size);if(b.getInt(strings+4)!=3)throw new IOException("Invalid native dependency string table");
                long stringOffset=layout.word(b,strings+layout.dataOffset()),stringBytes=layout.word(b,strings+layout.dataSize());range(b,stringOffset,stringBytes);boolean terminated=false;
                for(long at=data;at<data+bytes;at+=entrySize){long tag=layout.word(b,(int)at);if(tag==0){terminated=true;break;}if(tag!=1)continue;
                    long name=layout.word(b,(int)at+layout.wordSize());if(name<0||name>=stringBytes)throw new IOException("Invalid native dependency name");
                    int start=(int)(stringOffset+name),end=start,limit=(int)(stringOffset+stringBytes);
                    while(end<limit&&b.get(end)!=0){if(end-start>=255)throw new IOException("Native dependency name exceeds limit");end++;}
                    if(end==limit)throw new IOException("Unterminated native dependency name");
                    String text=new String(b.array(),start,end-start,StandardCharsets.UTF_8);
                    if(text.isEmpty()||text.equals(".")||text.equals("..")||text.indexOf('/')>=0||text.indexOf(92)>=0)throw new IOException("Invalid native dependency path");
                    if(dependencies.size()>=256)throw new IOException("Native dependency count exceeds limit");dependencies.add(text);
                }
                if(!terminated)throw new IOException("Unterminated native dynamic table");
            }
            if(type!=11)continue;
            long data=layout.word(b,header+layout.dataOffset()),bytes=layout.word(b,header+layout.dataSize()),entrySize=layout.word(b,header+layout.entrySize());
            int linked=b.getInt(header+layout.link());
            if(entrySize<layout.symbolSize()||bytes%entrySize!=0||bytes/entrySize>1_000_000||linked<0||linked>=count)throw new IOException("Invalid native symbol table");
            range(b,data,bytes);
            int strings=(int)(offset+(long)linked*size);
            if(b.getInt(strings+4)!=3)throw new IOException("Invalid native symbol string table");
            long stringOffset=layout.word(b,strings+layout.dataOffset()),stringBytes=layout.word(b,strings+layout.dataSize());range(b,stringOffset,stringBytes);
            for(long at=data;at<data+bytes;at+=entrySize){
                int p=(int)at,info=Byte.toUnsignedInt(b.get(p+layout.symbolInfo())),binding=info>>>4;
                if((binding!=1&&binding!=2)||Short.toUnsignedInt(b.getShort(p+layout.symbolSection()))==0||layout.word(b,p+layout.symbolValue())==0)continue;
                long name=Integer.toUnsignedLong(b.getInt(p));if(name>=stringBytes)throw new IOException("Invalid native symbol name");
                int start=(int)(stringOffset+name),end=start,limit=(int)(stringOffset+stringBytes);
                while(end<limit&&b.get(end)!=0)end++;
                if(end==limit)throw new IOException("Unterminated native symbol name");
                if(end-start>=5&&b.get(start)=='J'&&b.get(start+1)=='a'&&b.get(start+2)=='v'&&b.get(start+3)=='a'&&b.get(start+4)=='_')
                    exports.add(new String(b.array(),start,end-start,StandardCharsets.UTF_8));
            }
        }
        return new Metadata(Set.copyOf(exports),List.copyOf(dependencies));
    }
    private static void range(ByteBuffer b,long offset,long bytes)throws IOException {
        if(offset<0||bytes<0||offset>b.limit()||bytes>b.limit()-offset)throw new IOException("Native symbol table exceeds library bounds");
    }
}
