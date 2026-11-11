import api.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class ProtectedOverloadIdentifiersCaller {
    public static void call(int route,int shape,Root owner,long bits,float scale,Object arg,String name)throws Throwable {
        if(route==3){if(shape==0)owner.callback(bits,scale,(Callback)arg);else owner.tagged(bits,scale,(Token)arg);return;}
        Class input=shape==0?Callback.class:Token.class;
        Method method=Root.class.getDeclaredMethod(name,long.class,float.class,input);method.setAccessible(true);
        Trace.step(1);
        if(route==0){try{method.invoke(owner,bits,scale,arg);}catch(InvocationTargetException e){throw e.getCause();}}
        else {
            MethodHandle handle=MethodHandles.lookup().unreflect(method);
            if(route==1)handle.invokeWithArguments(owner,bits,scale,arg);
            else if(shape==0)handle.invokeExact(owner,bits,scale,(Callback)arg);
            else handle.invokeExact(owner,bits,scale,(Token)arg);
        }
        Trace.step(6);
    }
}
