package fixture;
public final class Core implements Api {
    public Result read(Object input, Box callback, long value) {
        return (Result)(Object)target(input, callback, value);
    }
    public Void target(Object input, Box<? extends Object> callback, long value) {
        Trace.hit(1);
        Object result = callback.apply(input, value);
        Trace.result = result;
        Trace.hit(3);
        return null;
    }
    public Result entry(Object input, Box callback, long value) { return read(input, callback, value); }
}
