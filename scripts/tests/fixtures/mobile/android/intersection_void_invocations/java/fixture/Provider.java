package fixture;
public final class Provider {
    static { Trace.hit(9); }
    public static <T extends First & Second> void enable(T value) {
        Trace.hit(1);
        value.first();
        Trace.hit(2);
        value.second();
        Trace.hit(3);
    }
}
