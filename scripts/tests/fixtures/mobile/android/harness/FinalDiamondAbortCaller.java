import fixture.*;
import java.lang.reflect.*;
public class FinalDiamondAbortCaller {
    public static Core make(Object seed,Object alternate) throws Throwable {
        try { return Core.class.getConstructor(Object.class,Object.class).newInstance(seed,alternate); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
}
