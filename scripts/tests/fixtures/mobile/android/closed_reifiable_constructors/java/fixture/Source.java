package fixture;
import java.util.Map;
public abstract class Source {
    public static class Key<E> {}
    public abstract Map<Key<?>, Object> data();
}
