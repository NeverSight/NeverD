import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class BoundedOwnerLabelHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned bounded label constructor"); }
    static Core make(int route,Object seed,Link converter,Object threshold) throws Throwable {
        try {
            if(route==0)return Client.make(seed,converter,threshold);
            if(route==1)return BoundedOwnerLabelCaller.make(seed,converter,threshold);
            if(route==2)return (Core)Core.class.getConstructor(Object.class,Link.class,Object.class).newInstance(seed,converter,threshold);
            return (Core)MethodHandles.publicLookup().findConstructor(Core.class,
                MethodType.methodType(void.class,Object.class,Link.class,Object.class)).invokeExact(seed,converter,threshold);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean synthetic=Boolean.parseBoolean(args[2]);
        Constructor<?> raw=Core.class.getConstructor(Object.class,Link.class,Object.class);
        require(raw.isSynthetic()==synthetic && raw.getGenericParameterTypes()[0]==Object.class && raw.getGenericParameterTypes()[1]==Link.class);
        TypeVariable<?>[] formals=Core.class.getTypeParameters();require(formals.length==2 && formals[0].getBounds()[0]==Object.class && formals[1].getBounds()[0]==Bound.class);
        Constructor<?> target=Core.class.getConstructor(Object.class,Link.class,Object.class,String.class);
        Type[] params=target.getGenericParameterTypes();require(params[0].equals(formals[0]) && params[2].equals(formals[0]));
        Type[] pair=((ParameterizedType)params[1]).getActualTypeArguments();require(pair[0].equals(formals[0])&&pair[1].equals(formals[1]));
        RuntimeException original=new IllegalArgumentException("owned label constructor failure");Core previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            Object seed=mode==1?null:new Object(),threshold=mode==2?null:new Object();
            Link converter=mode==3?null:new Link<Object,Bound>();
            Trace.events=0;Trace.partial=null;Trace.failure=original;Trace.failAt=mode>=4?mode-3:0;
            Core result=null;
            try {result=make(route,seed,converter,threshold);require(Trace.failAt==0);}
            catch(Throwable failure) {require(Trace.failAt!=0 && failure==original);}
            require(Trace.events==(mode==4?1:mode==5?12:mode==6?123:1234));
            require(Trace.partial instanceof Core);Core partial=(Core)Trace.partial;
            require(partial.seed==(mode==4?null:seed));
            require(partial.converter==(mode==4||mode==5?null:converter));
            require(partial.threshold==(mode==4||mode==5?null:threshold));
            require(partial.label==(mode==4||mode==5||mode==6?null:"owned label"));
            if(Trace.failAt==0)require(result==partial && result!=previous);
            previous=result;
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
