package maclauncher.android.bridge;

import java.io.IOException;
import java.nio.*;
import java.nio.file.*;
import java.util.*;

public final class SymbolNamesProbe {
    private static int checks;
    private static void check(boolean value){checks++;if(!value)throw new AssertionError("ELF metadata check "+checks);}
    private static void rejected(Path file,byte[] bytes,String message)throws Exception{
        Files.write(file,bytes);
        try{ElfNativeExports.readMetadata(file);throw new AssertionError("Malformed ELF accepted");}
        catch(IOException expected){check(expected.getMessage().contains(message));}
    }
    private static long word(ByteBuffer b,int at,boolean wide){return wide?b.getLong(at):Integer.toUnsignedLong(b.getInt(at));}
    private static void putWord(ByteBuffer b,int at,boolean wide,long value){if(wide)b.putLong(at,value);else b.putInt(at,(int)value);}
    public static void main(String[] args)throws Exception{
        Path directory=Files.createTempDirectory("android-elf-malformed-");
        try{
            for(String argument:args){
                Path source=Path.of(argument),bad=directory.resolve(source.getFileName());byte[] original=Files.readAllBytes(source);
                var metadata=ElfNativeExports.readMetadata(source);
                check(metadata.exports().equals(Set.of("Java_fixture_Long_"+"m".repeat(6200),"Java_fixture_Short_nativeCall")));
                check(metadata.dependencies().isEmpty());
                ByteBuffer b=ByteBuffer.wrap(original).order(ByteOrder.LITTLE_ENDIAN);boolean wide=b.get(4)==2;
                int off=(int)word(b,wide?40:32,wide),size=Short.toUnsignedInt(b.getShort(wide?58:46)),count=Short.toUnsignedInt(b.getShort(wide?60:48));
                int data=wide?24:16,length=wide?32:20,link=wide?40:24,entry=wide?56:36,info=wide?4:12,section=wide?6:14,value=wide?8:4;
                int symbols=0;for(int i=0;i<count;i++)if(b.getInt(off+i*size+4)==11){symbols=off+i*size;break;}
                check(symbols!=0);int strings=off+b.getInt(symbols+link)*size;
                int symbolStart=(int)word(b,symbols+data,wide),symbolBytes=(int)word(b,symbols+length,wide),symbolEntry=(int)word(b,symbols+entry,wide);
                int first=0;for(int at=symbolStart;at<symbolStart+symbolBytes;at+=symbolEntry){int binding=Byte.toUnsignedInt(b.get(at+info))>>>4;if((binding==1||binding==2)&&b.getShort(at+section)!=0&&word(b,at+value,wide)!=0){first=at;break;}}
                check(first!=0);
                byte[] changed=original.clone();ByteBuffer c=ByteBuffer.wrap(changed).order(ByteOrder.LITTLE_ENDIAN);
                putWord(c,strings+data,wide,wide?Long.MAX_VALUE:0xffff_ffffL);rejected(bad,changed,"exceeds library bounds");
                changed=original.clone();c=ByteBuffer.wrap(changed).order(ByteOrder.LITTLE_ENDIAN);
                putWord(c,strings+length,wide,wide?Long.MAX_VALUE:0xffff_ffffL);rejected(bad,changed,"exceeds library bounds");
                changed=original.clone();c=ByteBuffer.wrap(changed).order(ByteOrder.LITTLE_ENDIAN);c.putInt(symbols+link,count);rejected(bad,changed,"Invalid native symbol table");
                changed=original.clone();c=ByteBuffer.wrap(changed).order(ByteOrder.LITTLE_ENDIAN);c.putInt(first,(int)word(b,strings+length,wide));rejected(bad,changed,"Invalid native symbol name");
                int name=b.getInt(first),stringStart=(int)word(b,strings+data,wide),end=stringStart+name;
                while(original[end]!=0)end++;
                changed=original.clone();c=ByteBuffer.wrap(changed).order(ByteOrder.LITTLE_ENDIAN);putWord(c,strings+length,wide,end-stringStart);rejected(bad,changed,"Unterminated native symbol name");
                changed=original.clone();c=ByteBuffer.wrap(changed).order(ByteOrder.LITTLE_ENDIAN);putWord(c,symbols+entry,wide,wide?Long.MAX_VALUE:0xffff_ffffL);rejected(bad,changed,"Invalid native symbol table");
            }
            System.out.println("PASS real ELF32/ELF64 long ordinary and JNI names; bounds, overflow and termination controls ("+checks+" checks)");
        }finally{try(var paths=Files.list(directory)){for(Path file:paths.toList())Files.delete(file);}Files.delete(directory);}
    }
}
