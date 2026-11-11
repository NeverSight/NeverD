package fixture;
import java.io.IOException;
import java.util.Set;
public final class Entry {
    private Entry() {}
    public static Factory factory;
    public static <T> Set<Box<T>> query(T value, Class<?>... types) throws IOException {
        return factory.query(value, types);
    }
    public static <T> Set<Box<T>> named(T value, String context, Class<?>... types) throws IOException {
        return factory.named(value, context, types);
    }
}
