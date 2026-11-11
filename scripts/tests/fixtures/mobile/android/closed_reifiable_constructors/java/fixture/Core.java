package fixture;
import java.util.Map;
public final class Core {
    public static Core escaping;
    public static RuntimeException failure;
    public final Map<Source.Key<?>, Object> map;
    public final Object original;
    public final int size;
    public final boolean done;
    public Core(Source source) { this(source.data()); }
    public Core(Map<Source.Key<?>, ? extends Object> input) {
        escaping = this;
        map = (Map) input; original = input;
        size = input.size();
        if (failure != null) throw failure;
        done = true;
    }
}
