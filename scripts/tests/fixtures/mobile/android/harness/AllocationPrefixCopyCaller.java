import fixture.*;
import java.lang.reflect.*;
public class AllocationPrefixCopyCaller {
    public static Core make(Class<?> type,Object input,Object secondInput,double carry) throws Throwable {
        try { return Core.class.getConstructor(double.class,double.class,double.class,double.class,double.class,double.class,type,type).newInstance(carry,2.0,3.0,4.0,5.0,6.0,input,secondInput); }
        catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
}
