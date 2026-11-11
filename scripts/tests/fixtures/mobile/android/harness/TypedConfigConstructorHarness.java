import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class TypedConfigConstructorHarness {
    static void require(boolean ok){if(!ok)throw new AssertionError("owned typed config constructor");}
    static Core make(int route,Config config,float bias) throws Throwable {
        if(route==0)return Client.make(config,bias);
        if(route==1)return TypedConfigConstructorCaller.make(config,bias);
        if(route==2){try{return Core.class.getConstructor(Config.class,float.class).newInstance(config,bias);}catch(InvocationTargetException f){throw f.getCause();}}
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Config.class,float.class)).invokeExact(config,bias);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        TypeVariable<?> formal=Core.class.getTypeParameters()[0];
        for(Constructor<?> c:Core.class.getDeclaredConstructors()){
            ParameterizedType p=(ParameterizedType)c.getGenericParameterTypes()[0];require(p.getRawType()==Config.class && p.getActualTypeArguments()[0].equals(formal));
        }
        int[] bits={0,0x80000000,0x7f800000,0xff800000,0x7fc00000,0x7fc12345,0x3f800000,0x00000001,0x7f7fffff,0x3f000000,0x3f000000,0x3f000000,0x3f000000,0};
        for(int attempt=0;attempt<2;attempt++){
            Config config=mode==13?null:new Config(new Object());float bias=Float.intBitsToFloat(bits[mode]);
            Trace.partial=null;Trace.events=0;Trace.failAt=mode>=9&&mode<=12?mode-8:0;Trace.failure=new RuntimeException("owned config failure");
            Core result=null;Throwable failure=null;try{result=make(route,config,bias);}catch(Throwable f){failure=f;}
            int stage=Trace.failAt;
            require(stage==0?failure==null && result==Trace.partial:failure==Trace.failure && result==null);
            Core partial=(Core)Trace.partial;require(partial!=null && partial.config==(stage==1?null:config));
            require(Float.floatToRawIntBits(partial.bias)==(stage==1?0:stage==2||stage==3?0x7fc00000:bits[mode]));
            require(partial.config()==partial.config && ((View)partial).config()==partial.config);
            long expected=stage==1?1:stage==2?12:stage==3?123:1234;
            require(Trace.events==expected);
            Trace.events=0;Trace.failAt=0;Core primary=new Core(config);require(primary.config==config && Float.floatToRawIntBits(primary.bias)==0x7fc00000 && Trace.events==12);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
