package fixture;
import java.util.function.Supplier;
public final class Adapter implements Supplier {
    static { Trace.hit(3); }
    public Object snapshot;
    public Adapter() {
        snapshot = Trace.seed;
        Trace.adapter = this;
        Trace.hit(4);
    }
    public Object get() {
        Trace.hit(6);
        return snapshot;
    }
}
