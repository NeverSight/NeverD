package fixture;
public final class StaticCore {
    public static final Object first;
    public static final Object second;
    static {
        Trace.event(1);
        first = Trace.value;
        Trace.event(2);
        if (Trace.mode == 0) {
            Trace.event(3);
            second = Trace.value;
            Trace.event(4);
        } else {
            Trace.event(5);
            throw Trace.failure;
        }
    }
}
