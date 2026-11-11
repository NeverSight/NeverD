import fixture.*;
import kotlin.jvm.functions.Function2;
import kotlin.coroutines.Continuation;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class ScopedOwnerCompletionCaller {
    public static Object call(int route,Core owner,Object session,long bits,Function2 block,Continuation completion)throws Throwable {
        if(route==3)return owner.relay(session,bits,block,completion);
        if(route==0) {
            try {
                Method method=Core.class.getMethod("relay",Object.class,long.class,Function2.class,Continuation.class);
                return method.invoke(owner,session,bits,block,completion);
            }catch(InvocationTargetException error){throw error.getCause();}
        }
        MethodHandle handle=MethodHandles.publicLookup().findVirtual(Core.class,"relay",MethodType.methodType(Object.class,Object.class,long.class,Function2.class,Continuation.class));
        if(route==1)return handle.invokeWithArguments(owner,session,bits,block,completion);
        return (Object)handle.invokeExact(owner,session,bits,block,completion);
    }
}
