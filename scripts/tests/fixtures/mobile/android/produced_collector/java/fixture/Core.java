package fixture;
import java.util.function.Supplier;
import java.util.stream.Collector;
import java.util.stream.Collectors;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core {
    private Core() {}
    public static Collector returned(Supplier supplier) { return Factory.make(supplier); }
    public static Object ignored(Supplier supplier) { Factory.make(supplier); return Trace.result; }
    public static Collector standard(Supplier supplier) { return Collectors.toCollection(supplier); }
}
