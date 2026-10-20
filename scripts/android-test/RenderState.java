package maclauncher.test;

import android.opengl.EGL14;
import android.opengl.GLES20;
import android.opengl.GLES30;
import android.opengl.GLSurfaceView;
import android.view.View;
import android.view.ViewGroup;
import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import org.json.*;

/** On-demand, end-of-frame state from the application's current GL thread. */
public final class RenderState {
    private record Capture(long id,long deadline,String callSite,JSONObject texture,boolean capturing,JSONObject result) {}
    private static final AtomicLong serial=new AtomicLong();
    private static final AtomicReference<Capture> current=new AtomicReference<>();
    private static volatile boolean installed;
    static void installed(){installed=true;}
    public static JSONObject control(JSONObject request)throws JSONException {
        if(!installed)return json("available",false,"reason","Requires shaderTrace at launch");
        if(request.optBoolean("start"))current.set(new Capture(serial.incrementAndGet(),System.nanoTime()+3_000_000_000L,request.optString("callSite",""),textureRequest(request),false,null));
        return result();
    }
    /** Original GLSurfaceView owns execution and EGL context on both VMs. */
    static JSONObject queued(JSONObject request,View root)throws JSONException {
        if(!request.optBoolean("start"))return result();
        GLSurfaceView surface=surface(root,request.optInt("surfaceId",0));
        if(surface==null)return json("available",false,"reason","Selected Android window has no matching GLSurfaceView");
        Capture value=new Capture(serial.incrementAndGet(),System.nanoTime()+3_000_000_000L,"GLSurfaceView.queueEvent",textureRequest(request),false,null);
        current.set(value);surface.queueEvent(()->capture(value,"GLSurfaceView.queueEvent"));return result();
    }
    private static JSONObject result()throws JSONException {
        Capture value=current.get();if(value==null)return json("available",true,"pending",false);
        if(value.result==null&&System.nanoTime()>=value.deadline){var expired=new Capture(value.id,value.deadline,value.callSite,value.texture,false,json("available",false,"pending",false,"request",value.id,"reason","Original GL owner did not complete within three seconds"));current.compareAndSet(value,expired);value=current.get();}
        return value.result==null?json("available",true,"pending",true,"request",value.id,"callSite",value.callSite):new JSONObject(value.result.toString());
    }
    private static GLSurfaceView surface(View view,int id){
        if(view instanceof GLSurfaceView value&&(id==0||value.getId()==id))return value;
        if(view instanceof ViewGroup group)for(int i=0;i<group.getChildCount();i++){GLSurfaceView value=surface(group.getChildAt(i),id);if(value!=null)return value;}return null;
    }
    private static JSONObject textureRequest(JSONObject request)throws JSONException {
        JSONObject input=request.optJSONObject("texture");if(input==null)return null;
        int name=input.getInt("name"),width=input.getInt("width"),height=input.getInt("height");
        if(name<=0||width<1||width>8192||height<1||height>8192)throw new IllegalArgumentException("Texture observation requires a positive name and known dimensions1–8192 pixels");
        return json("name",name,"width",width,"height",height);
    }
    /** Injected immediately before an original Java EGL swap; no state setters. */
    public static void beforeSwap(String callSite){
        Capture request=current.get();
        if(request==null||request.result!=null||request.capturing||!request.callSite.isEmpty()&&!request.callSite.equals(callSite))return;
        capture(request,callSite);
    }
    private static void capture(Capture request,String callSite){
        var running=new Capture(request.id,request.deadline,request.callSite,request.texture,true,null);
        if(!current.compareAndSet(request,running))return;
        JSONObject captured;
        try{
            if(System.nanoTime()>=request.deadline)captured=json("available",false,"reason","Render-state request expired");
            else {captured=snapshot();if(request.texture!=null&&captured.optBoolean("available"))captured.put("textureObservation",texture(request.texture,captured.getString("version").startsWith("OpenGL ES 3")));}
            captured.put("request",request.id).put("pending",false).put("callSite",callSite);
            if(callSite.equals("GLSurfaceView.queueEvent"))captured.put("scope","original-GLSurfaceView-owner-context");
        }catch(Throwable failure){try{captured=json("available",false,"pending",false,"request",request.id,"error",failure.toString());}catch(JSONException impossible){return;}}
        current.compareAndSet(running,new Capture(request.id,request.deadline,request.callSite,request.texture,false,captured));
    }
    /** Small texel reads retain all modified bindings/pack state and never consume errors. */
    private static JSONObject texture(JSONObject request,boolean es3)throws JSONException {
        int name=request.getInt("name"),width=request.getInt("width"),height=request.getInt("height");
        if(!GLES20.glIsTexture(name))return json("available",false,"name",name,"reason","Texture name is not live in the current context");
        int bound=integer(GLES20.GL_TEXTURE_BINDING_2D),draw=integer(GLES20.GL_FRAMEBUFFER_BINDING),read=es3?integer(GLES30.GL_READ_FRAMEBUFFER_BINDING):draw,pack=integer(GLES20.GL_PACK_ALIGNMENT);
        int packBuffer=es3?integer(GLES30.GL_PIXEL_PACK_BUFFER_BINDING):0,row=es3?integer(GLES30.GL_PACK_ROW_LENGTH):0,skipRows=es3?integer(GLES30.GL_PACK_SKIP_ROWS):0,skipPixels=es3?integer(GLES30.GL_PACK_SKIP_PIXELS):0;
        int[] fbo={0};int target=es3?GLES30.GL_READ_FRAMEBUFFER:GLES20.GL_FRAMEBUFFER;
        try{
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,name);
            JSONObject out=json("available",true,"name",name,"suppliedExtent",new JSONArray(new int[]{width,height}),"scope","nine original texels; no texture mutation");
            for(var parameter:new int[]{GLES20.GL_TEXTURE_MIN_FILTER,GLES20.GL_TEXTURE_MAG_FILTER,GLES20.GL_TEXTURE_WRAP_S,GLES20.GL_TEXTURE_WRAP_T}){int[] value={0};GLES20.glGetTexParameteriv(GLES20.GL_TEXTURE_2D,parameter,value,0);out.put("parameter0x"+Integer.toHexString(parameter),value[0]);}
            GLES20.glGenFramebuffers(1,fbo,0);GLES20.glBindFramebuffer(target,fbo[0]);GLES20.glFramebufferTexture2D(target,GLES20.GL_COLOR_ATTACHMENT0,GLES20.GL_TEXTURE_2D,name,0);
            int status=GLES20.glCheckFramebufferStatus(target);out.put("framebufferStatus",status);
            if(status!=GLES20.GL_FRAMEBUFFER_COMPLETE)return out.put("available",false).put("reason","Original texture is not color-renderable");
            GLES20.glPixelStorei(GLES20.GL_PACK_ALIGNMENT,1);
            if(es3){GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER,0);GLES20.glPixelStorei(GLES30.GL_PACK_ROW_LENGTH,0);GLES20.glPixelStorei(GLES30.GL_PACK_SKIP_ROWS,0);GLES20.glPixelStorei(GLES30.GL_PACK_SKIP_PIXELS,0);}
            JSONArray pixels=new JSONArray();ByteBuffer bytes=ByteBuffer.allocateDirect(4);
            for(int y:new int[]{0,height/2,height-1})for(int x:new int[]{0,width/2,width-1}){bytes.clear();GLES20.glReadPixels(x,y,1,1,GLES20.GL_RGBA,GLES20.GL_UNSIGNED_BYTE,bytes);pixels.put(json("x",x,"y",y,"rgba",new JSONArray(new int[]{bytes.get(0)&255,bytes.get(1)&255,bytes.get(2)&255,bytes.get(3)&255})));}
            return out.put("texels",pixels);
        }finally{
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,bound);GLES20.glBindFramebuffer(target,read);if(es3)GLES20.glBindFramebuffer(GLES30.GL_DRAW_FRAMEBUFFER,draw);
            if(fbo[0]!=0)GLES20.glDeleteFramebuffers(1,fbo,0);GLES20.glPixelStorei(GLES20.GL_PACK_ALIGNMENT,pack);
            if(es3){GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER,packBuffer);GLES20.glPixelStorei(GLES30.GL_PACK_ROW_LENGTH,row);GLES20.glPixelStorei(GLES30.GL_PACK_SKIP_ROWS,skipRows);GLES20.glPixelStorei(GLES30.GL_PACK_SKIP_PIXELS,skipPixels);}
        }
    }
    private static JSONObject snapshot()throws JSONException {
        var context=EGL14.eglGetCurrentContext();
        if(context==null||context.equals(EGL14.EGL_NO_CONTEXT))return json("available",false,"reason","No current EGL context on swap thread");
        String version=GLES20.glGetString(GLES20.GL_VERSION);boolean es3=version!=null&&version.startsWith("OpenGL ES 3");
        int program=integer(GLES20.GL_CURRENT_PROGRAM),fbo=integer(GLES20.GL_FRAMEBUFFER_BINDING);
        JSONObject out=json("available",true,"pending",false,"capturedAt",System.currentTimeMillis(),"thread",Thread.currentThread().getName(),
            "scope","end-of-frame-current-context","perDraw",false,"context",context.toString(),"nativeEglContext",Long.toUnsignedString(context.getNativeHandle(),16),"version",version,
            "renderer",GLES20.glGetString(GLES20.GL_RENDERER),"program",program,"framebuffer",fbo,
            "viewport",integers(GLES20.GL_VIEWPORT,4),"scissor",integers(GLES20.GL_SCISSOR_BOX,4),
            "depth",json("enabled",GLES20.glIsEnabled(GLES20.GL_DEPTH_TEST),"function",integer(GLES20.GL_DEPTH_FUNC),"write",bool(GLES20.GL_DEPTH_WRITEMASK),"range",floats(GLES20.GL_DEPTH_RANGE,2)),
            "cull",json("enabled",GLES20.glIsEnabled(GLES20.GL_CULL_FACE),"mode",integer(GLES20.GL_CULL_FACE_MODE),"front",integer(GLES20.GL_FRONT_FACE)),
            "blend",json("enabled",GLES20.glIsEnabled(GLES20.GL_BLEND),"sourceRGB",integer(GLES20.GL_BLEND_SRC_RGB),"destinationRGB",integer(GLES20.GL_BLEND_DST_RGB),"sourceAlpha",integer(GLES20.GL_BLEND_SRC_ALPHA),"destinationAlpha",integer(GLES20.GL_BLEND_DST_ALPHA),"equationRGB",integer(GLES20.GL_BLEND_EQUATION_RGB),"equationAlpha",integer(GLES20.GL_BLEND_EQUATION_ALPHA)),
            "stencil",json("enabled",GLES20.glIsEnabled(GLES20.GL_STENCIL_TEST),"function",integer(GLES20.GL_STENCIL_FUNC),"reference",integer(GLES20.GL_STENCIL_REF),"valueMask",integer(GLES20.GL_STENCIL_VALUE_MASK),"writeMask",integer(GLES20.GL_STENCIL_WRITEMASK)),
            "scissorEnabled",GLES20.glIsEnabled(GLES20.GL_SCISSOR_TEST),"colorWrite",booleans(GLES20.GL_COLOR_WRITEMASK,4),
            "activeTextureUnit",integer(GLES20.GL_ACTIVE_TEXTURE)-GLES20.GL_TEXTURE0,
            "texture2D",integer(GLES20.GL_TEXTURE_BINDING_2D),"textureCube",integer(GLES20.GL_TEXTURE_BINDING_CUBE_MAP),
            "arrayBuffer",integer(GLES20.GL_ARRAY_BUFFER_BINDING),"elementBuffer",integer(GLES20.GL_ELEMENT_ARRAY_BUFFER_BINDING));
        if(es3)out.put("vertexArray",integer(GLES30.GL_VERTEX_ARRAY_BINDING)).put("readFramebuffer",integer(GLES30.GL_READ_FRAMEBUFFER_BINDING)).put("rasterizerDiscard",GLES20.glIsEnabled(GLES30.GL_RASTERIZER_DISCARD));
        if(program!=0&&GLES20.glIsProgram(program))out.put("programState",program(program));
        JSONArray attributes=new JSONArray();int maximum=integer(GLES20.GL_MAX_VERTEX_ATTRIBS);
        for(int index=0;index<Math.min(maximum,32);index++){
            int[] enabled={0};GLES20.glGetVertexAttribiv(index,GLES20.GL_VERTEX_ATTRIB_ARRAY_ENABLED,enabled,0);
            if(enabled[0]==0)continue;
            JSONObject attribute=json("index",index);
            int[] fields={GLES20.GL_VERTEX_ATTRIB_ARRAY_SIZE,GLES20.GL_VERTEX_ATTRIB_ARRAY_TYPE,GLES20.GL_VERTEX_ATTRIB_ARRAY_STRIDE,GLES20.GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,GLES20.GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING};
            String[] names={"size","type","stride","normalized","buffer"};
            for(int field=0;field<fields.length;field++){int[] value={0};GLES20.glGetVertexAttribiv(index,fields[field],value,0);attribute.put(names[field],value[0]);}
            if(es3){int[] divisor={0};GLES20.glGetVertexAttribiv(index,GLES30.GL_VERTEX_ATTRIB_ARRAY_DIVISOR,divisor,0);attribute.put("divisor",divisor[0]);}
            attributes.put(attribute);
        }
        return out.put("attributes",attributes).put("limits",json("attributes",32,"uniforms",64,"uniformArrayElements",1,"shaderSourceCharacters",8192,"textureUnits","current only"));
    }
    private static JSONObject program(int program)throws JSONException {
        int[] status={0};GLES20.glGetProgramiv(program,GLES20.GL_LINK_STATUS,status,0);
        JSONObject out=json("linked",status[0]!=0,"log",bounded(GLES20.glGetProgramInfoLog(program),8192));
        int[] count={0},attached=new int[8];GLES20.glGetAttachedShaders(program,attached.length,count,0,attached,0);
        JSONArray shaders=new JSONArray();
        for(int index=0;index<count[0];index++){
            int shader=attached[index];int[] type={0};GLES20.glGetShaderiv(shader,GLES20.GL_SHADER_TYPE,type,0);GLES20.glGetShaderiv(shader,GLES20.GL_COMPILE_STATUS,status,0);
            shaders.put(json("id",shader,"type",type[0],"compiled",status[0]!=0,"source",bounded(GLES20.glGetShaderSource(shader),8192),"log",bounded(GLES20.glGetShaderInfoLog(shader),8192)));
        }
        out.put("shaders",shaders);GLES20.glGetProgramiv(program,GLES20.GL_ACTIVE_UNIFORMS,count,0);out.put("activeUniforms",count[0]);
        JSONArray uniforms=new JSONArray();
        for(int index=0;index<Math.min(count[0],64);index++){
            int[] size={0},type={0};String name=GLES20.glGetActiveUniform(program,index,size,0,type,0);int location=GLES20.glGetUniformLocation(program,name);
            JSONObject value=json("name",name,"type",type[0],"arraySize",size[0],"location",location);
            int components=components(type[0]);
            if(location>=0&&components>0){
                if(type[0]==GLES20.GL_FLOAT||type[0]>=GLES20.GL_FLOAT_VEC2&&type[0]<=GLES20.GL_FLOAT_VEC4||type[0]>=GLES20.GL_FLOAT_MAT2&&type[0]<=GLES20.GL_FLOAT_MAT4){
                    float[] data=new float[components];GLES20.glGetUniformfv(program,location,data,0);value.put("firstElement",floatArray(data));
                }else{int[] data=new int[components];GLES20.glGetUniformiv(program,location,data,0);value.put("firstElement",new JSONArray(data));}
            }else value.put("valuesAvailable",false);
            uniforms.put(value);
        }
        return out.put("uniforms",uniforms);
    }
    private static int components(int type){return switch(type){
        case GLES20.GL_FLOAT,GLES20.GL_INT,GLES20.GL_BOOL,GLES20.GL_SAMPLER_2D,GLES20.GL_SAMPLER_CUBE->1;
        case GLES20.GL_FLOAT_VEC2,GLES20.GL_INT_VEC2,GLES20.GL_BOOL_VEC2->2;
        case GLES20.GL_FLOAT_VEC3,GLES20.GL_INT_VEC3,GLES20.GL_BOOL_VEC3->3;
        case GLES20.GL_FLOAT_VEC4,GLES20.GL_INT_VEC4,GLES20.GL_BOOL_VEC4,GLES20.GL_FLOAT_MAT2->4;
        case GLES20.GL_FLOAT_MAT3->9;case GLES20.GL_FLOAT_MAT4->16;default->0;
    };}
    private static int integer(int key){int[] value={0};GLES20.glGetIntegerv(key,value,0);return value[0];}
    private static JSONArray integers(int key,int count)throws JSONException {int[] value=new int[count];GLES20.glGetIntegerv(key,value,0);return new JSONArray(value);}
    private static JSONArray floats(int key,int count)throws JSONException {float[] value=new float[count];GLES20.glGetFloatv(key,value,0);return floatArray(value);}
    private static JSONArray floatArray(float[] values)throws JSONException {JSONArray out=new JSONArray();for(float value:values)out.put(Float.isFinite(value)?(Object)value:Float.toString(value));return out;}
    private static boolean bool(int key){boolean[] value={false};GLES20.glGetBooleanv(key,value,0);return value[0];}
    private static JSONArray booleans(int key,int count)throws JSONException {boolean[] value=new boolean[count];GLES20.glGetBooleanv(key,value,0);return new JSONArray(value);}
    private static String bounded(String value,int limit){return value==null?"":value.substring(0,Math.min(value.length(),limit));}
    private static JSONObject json(Object...values)throws JSONException {JSONObject out=new JSONObject();for(int i=0;i<values.length;i+=2)out.put((String)values[i],values[i+1]);return out;}
}
