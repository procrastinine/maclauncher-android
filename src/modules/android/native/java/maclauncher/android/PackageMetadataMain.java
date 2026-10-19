package maclauncher.android;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import org.json.*;
import maclauncher.android.framework.PackageMetadata;

/** A reusable headless metadata process. No guest classes or lifecycle callbacks run here. */
public final class PackageMetadataMain {
    public static void main(String[] args) {
        int status=0;
        try {
            if(args.length!=2)throw new IllegalArgumentException("Expected runtime and temporary directories");
            Path runtime=Path.of(args[0]).toRealPath(),root=Path.of(args[1]).toRealPath();
            Path files=Files.createDirectories(root.resolve("files")),cache=Files.createDirectories(root.resolve("cache"));
            RuntimeBootstrap.initialize(runtime,root,files,cache,"maclauncher.metadata",false,false,false);
            PackageMetadata.initialize(runtime);
            try(var input=new BufferedReader(new InputStreamReader(System.in,StandardCharsets.UTF_8))) {
                String line;
                while((line=input.readLine())!=null) {
                    JSONObject request=new JSONObject(line),reply=new JSONObject().put("id",request.getLong("id"));
                    try {
                        var sources=new ArrayList<Path>();JSONArray paths=request.getJSONArray("sources");
                        for(int i=0;i<paths.length();i++)sources.add(Path.of(paths.getString(i)).toRealPath());
                        Path icon=root.resolve("icon-"+request.getLong("id")+".png");
                        reply.put("result",PackageMetadata.read(sources,cache,request.getString("locale"),icon));
                    }catch(Exception failure){reply.put("error",failure.toString());}
                    System.out.println("[android-package-metadata]"+reply);System.out.flush();
                }
            }
        }catch(Throwable failure){failure.printStackTrace();status=1;}
        System.exit(status);
    }
}
