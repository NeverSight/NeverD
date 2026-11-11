package fixture;
public final class Caller {
    public static Box<?> run(Provider<Object> provider, Box<Object> input, Object tag, long bits, double value) {
        Trace.hit(1);
        Box<?> result = provider.read(input, tag, bits, value);
        Trace.hit(6);
        return result;
    }
    public static Box<?> through(Api<Object> provider, Box<Object> input, Object tag, long bits, double value) {
        Trace.hit(1);
        Box<?> result = provider.read(input, tag, bits, value);
        Trace.hit(6);
        return result;
    }
    public static Class<?> token(Class<?> candidate, Class<?> parent) {
        Trace.hit(6);
        Class<?> result = candidate.asSubclass(parent);
        Trace.hit(7);
        return result;
    }
}
