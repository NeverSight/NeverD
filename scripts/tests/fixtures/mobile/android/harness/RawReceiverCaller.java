import fixture.*;
public final class RawReceiverCaller {
    public static Box<?> run(Provider<Object> provider, Box<Object> input, Object tag, long bits, double value) {
        return Caller.run(provider, input, tag, bits, value);
    }
    public static Box<?> through(Api<Object> provider, Box<Object> input, Object tag, long bits, double value) {
        return Caller.through(provider, input, tag, bits, value);
    }
    public static Class<?> token(Class<?> candidate, Class<?> parent) { return Caller.token(candidate, parent); }
}
