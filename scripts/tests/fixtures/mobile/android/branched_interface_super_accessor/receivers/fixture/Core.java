package fixture;
public final class Core {
    public static int call(Child receiver, long value) {
        Trace.hit(6);
        int result = Child.DefaultImpls.measure(receiver, value);
        Trace.hit(0);
        return result;
    }
}
