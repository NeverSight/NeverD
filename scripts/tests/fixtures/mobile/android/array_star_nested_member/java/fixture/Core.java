package fixture;
import java.io.IOException;
import java.util.Set;
public final class Core {
    private Core() {}
    public static <Q> Set<Box<Q>> direct(Factory factory, Q value, Class<?>... types) throws IOException {
        return factory.query(value, types);
    }
    public static <Q> Set<Box<Q>> concrete(Impl factory, Q value, Class<?>... types) throws IOException {
        return factory.query(value, types);
    }
    public static <Q> Set<Box<Q>> wrapped(Factory factory, Q value, Class<?>... types) throws IOException {
        try { return factory.query(value, types); }
        catch (IOException failure) {
            Trace.caught = failure;
            Trace.handled++;
            throw failure;
        } finally { Trace.hit(4); }
    }
    public static <Q> Set<Box<Q>> named(Factory factory, Q value, String context, Class<?>... types) throws IOException {
        return factory.named(value, context, types);
    }

    public static <Q> Result routed(Factory ignored, Q value, Class<?>... types) throws IOException {
        return collect(Entry.query(value, types));
    }
    public static <Q> Result routedNamed(Factory ignored, Q value, String context, Class<?>... types) throws IOException {
        return collect(Entry.named(value, context, types));
    }
    private static <Q> Result collect(Set<Box<Q>> values) {
        Trace.collections++;
        Trace.collection = values;
        Trace.events = Trace.events * 10 + 5;
        return new Result(values);
    }

}
