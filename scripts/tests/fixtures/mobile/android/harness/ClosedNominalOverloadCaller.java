import fixture.*;
import kotlin.jvm.functions.Function2;
import kotlin.jvm.internal.DefaultConstructorMarker;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class ClosedNominalOverloadCaller {
    public static Core make(int route,int kind,Object first,Object second,Object[] rest,Handler handler,Function2 block,int mask)throws Throwable {
        if(route==3)return kind==0?new Invoker().make(first,second,rest,handler,mask):new Core(first,second,rest,block);
        if(route==0) {
            try {
                if(kind==0)return Core.class.getConstructor(Object.class,Object.class,Object[].class,Handler.class,int.class,DefaultConstructorMarker.class).newInstance(first,second,rest,handler,mask,null);
                return Core.class.getConstructor(Object.class,Object.class,Object[].class,Function2.class).newInstance(first,second,rest,block);
            }catch(InvocationTargetException error){throw error.getCause();}
        }
        MethodType type=kind==0?MethodType.methodType(void.class,Object.class,Object.class,Object[].class,Handler.class,int.class,DefaultConstructorMarker.class):MethodType.methodType(void.class,Object.class,Object.class,Object[].class,Function2.class);
        MethodHandle handle=MethodHandles.publicLookup().findConstructor(Core.class,type);
        if(route==1) {
            if(kind==0)return (Core)handle.invokeWithArguments(first,second,rest,handler,mask,null);
            return (Core)handle.invokeWithArguments(first,second,rest,block);
        }
        if(kind==0)return (Core)handle.invokeExact(first,second,rest,handler,mask,(DefaultConstructorMarker)null);
        return (Core)handle.invokeExact(first,second,rest,block);
    }
}
