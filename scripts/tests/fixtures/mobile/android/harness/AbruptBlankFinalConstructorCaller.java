import fixture.Core;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;

public class AbruptBlankFinalConstructorCaller {
    public static Core call(int shape, Throwable failure, Object value,
                            long number, boolean enabled) throws Throwable {
        if (shape == 2) return new Core(value, number, enabled);
        Constructor<Core> constructor = shape == 0
            ? Core.class.getDeclaredConstructor()
            : Core.class.getDeclaredConstructor(Throwable.class, long.class);
        constructor.setAccessible(true);
        try {
            return shape == 0 ? constructor.newInstance()
                : constructor.newInstance(failure, number);
        } catch (InvocationTargetException error) {
            throw error.getCause();
        }
    }
}
