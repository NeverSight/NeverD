package fixture;
public class Outer extends Bound {
    protected static class Hidden<T extends Outer> extends Mid<T> {
        public boolean run(Context context, T input, int bits) {
            Trace.hit(1); Trace.receiver = this; Trace.context = context;
            Trace.input = input; Trace.bits = bits; Trace.hit(2);
            Trace.action.run(); Trace.hit(3); return Trace.flag;
        }
    }
    public static class Facade extends Hidden<Outer> {}
}
