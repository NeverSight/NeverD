import fixture.*;
import kotlin.jvm.functions.Function2;
import kotlin.coroutines.Continuation;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class ScopedSuspendForwarderCaller {
    public static Object call(int route,int kind,Scope owner,long bits,Function2 block,Continuation completion)throws Throwable {
        if(route==3)return kind==0?owner.runWindow(bits,block,completion):owner.runGauge(Double.longBitsToDouble(bits),block,completion);
        String name=kind==0?"runWindow":"runGauge";Class scalar=kind==0?long.class:double.class;
        if(route==0) {
            try {
                Method method=Scope.class.getMethod(name,scalar,Function2.class,Continuation.class);
                if(kind==0)return method.invoke(owner,bits,block,completion);
                return method.invoke(owner,Double.longBitsToDouble(bits),block,completion);
            }catch(InvocationTargetException error){throw error.getCause();}
        }
        MethodHandle handle=MethodHandles.publicLookup().findVirtual(Scope.class,name,MethodType.methodType(Object.class,scalar,Function2.class,Continuation.class));
        if(route==1) {
            if(kind==0)return handle.invokeWithArguments(owner,bits,block,completion);
            return handle.invokeWithArguments(owner,Double.longBitsToDouble(bits),block,completion);
        }
        if(kind==0)return (Object)handle.invokeExact(owner,bits,block,completion);
        return (Object)handle.invokeExact(owner,Double.longBitsToDouble(bits),block,completion);
    }
}
