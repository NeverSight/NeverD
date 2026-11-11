import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class SyntheticInterfaceRootCaller {
    public static void invoke(int route,Api receiver,int kind,boolean defaults,String name,Object token,long a,long b,long c,long d,float scale,int mode) throws Throwable {
        if(route==0||route==1) {
            Class<?> owner=route==0?Api.class:kind==0?Alpha.class:Beta.class;
            try { owner.getMethod(name,Object.class,long.class,long.class,long.class,long.class,float.class,int.class).invoke(receiver,token,a,b,c,d,scale,mode); }
            catch(InvocationTargetException failure){throw failure.getCause();}
        } else if(route==2) {
            MethodHandle h=MethodHandles.publicLookup().findVirtual(Api.class,name,MethodType.methodType(void.class,Object.class,long.class,long.class,long.class,long.class,float.class,int.class));
            h.invokeExact(receiver,token,a,b,c,d,scale,mode);
        } else {
            Invoker call=new Invoker();
            if(defaults)call.defaults(receiver,token,a,b,c,d,scale,mode);
            else if(kind==0)call.concrete((Alpha)receiver,token,a,b,c,d,scale,mode);
            else call.branch((Branch)receiver,token,a,b,c,d,scale,mode);
        }
    }
}
