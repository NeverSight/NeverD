package fixture;
public class Base<V extends Bound> {
    public boolean run(Context context, V value, int bits) {
        Trace.hit(1); Trace.receiver = this;
        Trace.context = context; Trace.value = value; Trace.bits = bits;
        Trace.hit(2); Trace.action.run(); Trace.hit(4);
        return Trace.result;
    }
}
