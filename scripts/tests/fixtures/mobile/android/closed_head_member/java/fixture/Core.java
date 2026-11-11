package fixture;
import java.io.IOException;
import java.util.Map;
public final class Core {
    private Core() {}
    public static <Q> Map<String, Q> direct(Factory factory, Class<Q> type) throws IOException {
        return factory.query(type);
    }
    public static <Q> Map<String, Q> concrete(Impl factory, Class<Q> type) throws IOException {
        return factory.query(type);
    }
    public static String pick(Factory factory, Class<String> type) throws IOException {
        return factory.query(type).get("key");
    }
    public static <Q> Map<String, Q> wrapped(Factory factory, Class<Q> type) throws IOException {
        try { return factory.query(type); }
        catch (IOException failure) {
            Trace.caught = failure;
            Trace.handled++;
            throw failure;
        }
    }
}
