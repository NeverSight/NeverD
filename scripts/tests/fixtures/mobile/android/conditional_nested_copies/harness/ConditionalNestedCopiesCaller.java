import fixture.*;
import java.lang.reflect.InvocationTargetException;
public final class ConditionalNestedCopiesCaller {
    public static Outer run(int variant, int route, boolean choose, Object tag, long first, long second) throws Throwable {
        if (route == 0)
            return variant == 0 ? Core.make(choose, tag, first, second)
                : Core.floating(choose, tag, Double.longBitsToDouble(first), Double.longBitsToDouble(second));
        try {
            Class<?> scalar = variant == 0 ? long.class : double.class;
            Object a, b;
            if (variant == 0) { a = Long.valueOf(first); b = Long.valueOf(second); }
            else { a = Double.valueOf(Double.longBitsToDouble(first)); b = Double.valueOf(Double.longBitsToDouble(second)); }
            return (Outer) Core.class.getMethod(variant == 0 ? "make" : "floating", boolean.class,
                Object.class, scalar, scalar).invoke(null, choose, tag, a, b);
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
}
