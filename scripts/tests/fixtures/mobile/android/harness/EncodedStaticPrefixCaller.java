import fixture.Core;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class EncodedStaticPrefixCaller {
    public static Core make(int route,int a,long b,float c,double d,Object token,int mask)throws Throwable {
        if(route==0)return new Core(a,b,c,d,token,mask);
        if(route==3)return new Core();
        if(route==1) {
            try{return Core.class.getConstructor(int.class,long.class,float.class,double.class,Object.class,int.class).newInstance(a,b,c,d,token,mask);}
            catch(InvocationTargetException failure){throw failure.getCause();}
        }
        MethodHandle h=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,int.class,long.class,float.class,double.class,Object.class,int.class));
        return (Core)h.invokeExact(a,b,c,d,token,mask);
    }
}
