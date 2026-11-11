import fixture.*;
import kotlin.jvm.functions.Function1;
import kotlin.jvm.internal.DefaultConstructorMarker;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class LocalStaticPrefixCaller {
    public static Core<?> make(int route,int count,int capacity,Function1 first,Function1 second,int mask)throws Throwable {
        if(route==3)return new Invoker().make(count);
        if(route==0) {
            try{return (Core<?>)Core.class.getConstructor(int.class,int.class,Function1.class,Function1.class,int.class,DefaultConstructorMarker.class).newInstance(count,capacity,first,second,mask,null);}
            catch(InvocationTargetException failure){throw failure.getCause();}
        }
        MethodHandle h=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,int.class,int.class,Function1.class,Function1.class,int.class,DefaultConstructorMarker.class));
        if(route==1)return (Core<?>)h.invokeWithArguments(count,capacity,first,second,mask,null);
        return (Core<?>)h.invokeExact(count,capacity,first,second,mask,(DefaultConstructorMarker)null);
    }
}
