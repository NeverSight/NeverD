import fixture.*;
import java.lang.reflect.*;
public class ConstructorInstanceReadCaller {
    public static Core make(boolean foreign,Object seed,Object[] array,Object context) throws Throwable {
        try { return (Core)Core.class.getConstructor(foreign ? Values.class : Core.class,Object[].class,Object.class)
                .newInstance(seed,array,context); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
}
