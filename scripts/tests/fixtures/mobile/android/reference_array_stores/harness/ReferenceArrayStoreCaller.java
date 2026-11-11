import fixture.*;
import java.lang.reflect.InvocationTargetException;
public final class ReferenceArrayStoreCaller {
    public static Object run(int variant, int route, Object[] array, Object value, int length, int index,
                             Object tag, Object[] values) throws Throwable {
        if (route == 0) {
            if (variant == 0) return Core.direct(array, index, value);
            if (variant == 1) return Core.fresh(value, length, index);
            if (variant == 2) return Core.nested(value, length, index, tag);
            if (variant == 3) return Core.rows(array, index, value);
            return Core.loop(values);
        }
        try {
            if (variant == 0 || variant == 3)
                return Core.class.getMethod(variant == 0 ? "direct" : "rows", Object[].class, int.class, Object.class)
                    .invoke(null, array, index, value);
            if (variant == 1)
                return Core.class.getMethod("fresh", Object.class, int.class, int.class).invoke(null, value, length, index);
            if (variant == 2)
                return Core.class.getMethod("nested", Object.class, int.class, int.class, Object.class)
                    .invoke(null, value, length, index, tag);
            return Core.class.getMethod("loop", Object[].class).invoke(null, (Object) values);
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
}
