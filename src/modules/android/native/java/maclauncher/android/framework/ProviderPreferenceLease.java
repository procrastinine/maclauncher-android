package maclauncher.android.framework;

import android.content.SharedPreferences;
import android.util.AtomicFile;
import java.io.*;

/** Restores one explicit provider operation's preference, including after process death. */
final class ProviderPreferenceLease implements AutoCloseable {
    private final SharedPreferences preferences;
    private final String key;
    private final AtomicFile journal;
    private ProviderPreferenceLease(SharedPreferences preferences,String key,File file){this.preferences=preferences;this.key=key;journal=new AtomicFile(file);}
    static ProviderPreferenceLease open(SharedPreferences preferences,String key,File file,boolean enable)throws IOException{
        var lease=new ProviderPreferenceLease(preferences,key,file);
        lease.restore();
        if(!enable)return lease;
        FileOutputStream output=null;
        try{
            output=lease.journal.startWrite();
            var data=new DataOutputStream(output);data.writeInt(1);data.writeBoolean(preferences.contains(key));data.writeBoolean(preferences.getBoolean(key,false));data.flush();
            lease.journal.finishWrite(output);output=null;
            if(!preferences.edit().putBoolean(key,true).commit())throw new IOException("Google library preference could not be saved");
            return lease;
        }catch(IOException|RuntimeException failure){if(output!=null)lease.journal.failWrite(output);lease.restore();throw failure;}
    }
    private void restore()throws IOException{
        boolean present,value;
        try(var input=new DataInputStream(journal.openRead())){
            if(input.readInt()!=1)throw new IOException("Google library operation recovery is invalid");
            present=input.readBoolean();value=input.readBoolean();
            if(input.read()!=-1)throw new IOException("Google library operation recovery is invalid");
        }catch(FileNotFoundException absent){return;}
        var editor=preferences.edit();if(present)editor.putBoolean(key,value);else editor.remove(key);
        if(!editor.commit())throw new IOException("Google library preference could not be restored");
        journal.delete();
    }
    @Override public void close()throws IOException{restore();}
}
