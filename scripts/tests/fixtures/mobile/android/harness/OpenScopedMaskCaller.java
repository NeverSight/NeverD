import fixture.Span;
import java.lang.reflect.InvocationTargetException;
import kotlin.jvm.internal.DefaultConstructorMarker;
public class OpenScopedMaskCaller {
    public static Span<?> make(float start, float end, Object data, int mask) throws Throwable {
        try {
            return (Span<?>)Span.class.getDeclaredConstructor(float.class, float.class, Object.class,
                    int.class, DefaultConstructorMarker.class).newInstance(start, end, data, mask, null);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
}
