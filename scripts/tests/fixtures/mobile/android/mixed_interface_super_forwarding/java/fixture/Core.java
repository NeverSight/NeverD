package fixture;
public final class Core {
    public static Api make() { return new Owner(); }
    public static int call(Api owner, Context context, Input input, int value) {
        Trace.hit(6);
        int result = owner.measure(context, input, value);
        Trace.hit(0);
        return result;
    }
    public static long call(Api owner, Context context, Input input, long value) {
        Trace.hit(6);
        long result = owner.measure(context, input, value);
        Trace.hit(0);
        return result;
    }
}
