import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class NominalSuperConstructorCaller {
    public static Core make(int route,Token value,long stamp)throws Throwable {
        if(route==3)return new Core(value,stamp);
        if(route==0){
            try{return Core.class.getConstructor(Token.class,long.class).newInstance(value,stamp);}
            catch(InvocationTargetException failure){throw failure.getCause();}
        }
        MethodHandle h=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Token.class,long.class));
        if(route==1)return (Core)h.invokeWithArguments(value,stamp);
        return (Core)h.invokeExact(value,stamp);
    }
}
