import fixture.*;
import java.lang.reflect.*;
import kotlin.jvm.internal.DefaultConstructorMarker;
public class ScopedMaskConstructorsCaller {
    public static Core<?> make(int count, int capacity, Sink<?> callback, int mask) throws Throwable {
        try { return (Core<?>) Core.class.getDeclaredConstructor(int.class, int.class, Sink.class, int.class,
                DefaultConstructorMarker.class).newInstance(count, capacity, callback, mask, null); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
}
