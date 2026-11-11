package fixture;
import java.util.List;
public final class Core {
    public final Object value;
    public Core(Object input, int marker) { this((List<Leaf>)input); }
    public Core(Object input, Leaf... ignored) { this((List<Leaf>)input); }
    public Core(List<Leaf> values) {
        Trace.hit(2, this);
        this.value = values;
        Trace.core = this;
        Trace.hit(3, this);
    }
    public Core(Leaf... values) {
        Trace.hit(4, this);
        this.value = values;
        Trace.core = this;
        Trace.hit(5, this);
    }
}
