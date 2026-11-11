package fixture;
import java.util.function.Supplier;
import java.util.stream.Collector;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core {
    private Core() {}
    public static Collector returned(Supplier supplier) { return Factory.make(supplier); }
    public static Object ignored(Supplier supplier) { Factory.make(supplier); return Trace.result; }
    public static Collector fromArray(Supplier[] suppliers) { return Factory.make(suppliers[1]); }
}
