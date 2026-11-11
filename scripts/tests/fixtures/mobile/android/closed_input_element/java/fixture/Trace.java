package fixture;
import java.util.Set;
public abstract class Trace {
    public static Trace hook;
    public static Factory<?> factory;
    public static Class<? extends Factory<?>> next;
    public static Object value;
    public Trace() {}
    public abstract void observe(int phase, Subject target, Class<?> token, Set<Class<?>> seen);
    public static void hit(int phase, Subject target, Class<?> token, Set<Class<?>> seen) {
        if (hook != null) hook.observe(phase, target, token, seen);
    }
}
