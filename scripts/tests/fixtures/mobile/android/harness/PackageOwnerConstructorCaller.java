import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class PackageOwnerConstructorCaller {
    public static Core call(int route,int kind,int hint,Policy policy)throws Throwable {
        if(route==3)return kind==0?new Core():kind==1?new Core(hint):new Core(policy);
        Class[] types=kind==0?new Class[]{}:kind==1?new Class[]{int.class}:new Class[]{Policy.class};
        Object[] args=kind==0?new Object[]{}:kind==1?new Object[]{hint}:new Object[]{policy};
        if(route==0){try{return Core.class.getConstructor(types).newInstance(args);}catch(InvocationTargetException e){throw e.getCause();}}
        MethodHandle handle=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,types));
        if(route==1)return (Core)handle.invokeWithArguments(args);
        return kind==0?(Core)handle.invokeExact():kind==1?(Core)handle.invokeExact(hint):(Core)handle.invokeExact(policy);
    }
}
