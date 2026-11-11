package fixture;
public final class Provider {
    static {
        Trace.hit(2);
        if (Trace.holder != null) {
            Trace.holder.first = Trace.replaced;
            Trace.holder.second = Trace.replaced;
        }
    }
    public static Object read(Object input) {
        Trace.calls++;
        Trace.seen = input;
        Trace.hit(3);
        if (input != null) ((int[]) input)[0]++;
        return Trace.result;
    }
}
