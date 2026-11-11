import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class NamedDefaultAccessorCaller {
    public static void invoke(int route,Child receiver,String access,String forward,Object token,long a,long b,long c,long d,float scale,int mode) throws Throwable {
        if(route==0) new Invoker().through(receiver,token,a,b,c,d,scale,mode);
        else if(route==1||route==2) {
            Class<?> owner=route==1?Class.forName("fixture.Child$DefaultImpls"):Child.class;
            try { owner.getDeclaredMethod(route==1?forward:access,Child.class,Object.class,long.class,long.class,long.class,long.class,float.class,int.class).invoke(null,receiver,token,a,b,c,d,scale,mode); }
            catch(InvocationTargetException failure){throw failure.getCause();}
        } else {
            MethodHandle h=MethodHandles.publicLookup().findStatic(Child.class,access,MethodType.methodType(void.class,Child.class,Object.class,long.class,long.class,long.class,long.class,float.class,int.class));
            h.invokeExact(receiver,token,a,b,c,d,scale,mode);
        }
    }
}
