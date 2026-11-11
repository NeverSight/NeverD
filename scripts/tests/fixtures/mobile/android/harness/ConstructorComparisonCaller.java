import fixture.*;
import java.lang.reflect.*;
public class ConstructorComparisonCaller {
    public static Core make(Class<?>[] types,Object[] values) throws Throwable {
        try { return (Core)Core.class.getConstructor(types).newInstance(values); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
}
