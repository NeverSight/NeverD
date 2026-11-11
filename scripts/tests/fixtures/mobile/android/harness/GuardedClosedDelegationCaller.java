import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.List;
public final class GuardedClosedDelegationCaller {
    public static Core make(int route,int mode,List<?> first,List<?> second,long wide,float scale,int mask)throws Throwable {
        Class<?>[] types=mode==44?new Class<?>[0]:new Class<?>[]{List.class,List.class,long.class,float.class,int.class,Marker.class};
        switch(route){
            case 0:
                try{
                    Constructor<Core> c=Core.class.getConstructor(types);
                    return mode==44?c.newInstance():c.newInstance(first,second,wide,scale,mask,null);
                }catch(InvocationTargetException e){throw e.getCause();}
            case 1:{
                MethodHandle h=MethodHandles.lookup().findConstructor(Core.class,MethodType.methodType(void.class,types));
                return mode==44?(Core)h.invokeWithArguments():(Core)h.invokeWithArguments(first,second,wide,scale,mask,null);
            }
            case 2:{
                MethodHandle h=MethodHandles.lookup().findConstructor(Core.class,MethodType.methodType(void.class,types));
                return mode==44?(Core)h.invokeExact():(Core)h.invokeExact((List)first,(List)second,wide,scale,mask,(Marker)null);
            }
            default:return mode==44?new Core():new Core((List)first,(List)second,wide,scale,mask,null);
        }
    }
}
