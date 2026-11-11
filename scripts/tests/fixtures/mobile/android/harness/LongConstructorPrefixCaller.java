import fixture.*;
import kotlin.jvm.internal.DefaultConstructorMarker;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class LongConstructorPrefixCaller {
    public static Core make(int route,Shape image,long offset,long size,int mask)throws Throwable {
        if(route==3)return new Invoker().make(image,offset,size,mask);
        if(route==0){
            try{return (Core)Core.class.getConstructor(Shape.class,long.class,long.class,int.class,DefaultConstructorMarker.class).newInstance(image,offset,size,mask,null);}
            catch(InvocationTargetException failure){throw failure.getCause();}
        }
        MethodHandle h=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Shape.class,long.class,long.class,int.class,DefaultConstructorMarker.class));
        if(route==1)return (Core)h.invokeWithArguments(image,offset,size,mask,null);
        return (Core)h.invokeExact(image,offset,size,mask,(DefaultConstructorMarker)null);
    }
}
