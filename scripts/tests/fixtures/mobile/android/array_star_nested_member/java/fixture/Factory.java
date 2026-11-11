package fixture;
import java.io.IOException;
import java.util.Set;
public interface Factory {
    <T> Set<Box<T>> query(T value, Class<?>... types) throws IOException;
    <T> Set<Box<T>> named(T value, String context, Class<?>... types) throws IOException;
}
