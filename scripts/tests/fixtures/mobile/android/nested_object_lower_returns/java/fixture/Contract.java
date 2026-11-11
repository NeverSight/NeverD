package fixture;
import java.util.Iterator;
public interface Contract<E> {
    default Iterator<E> values() { return null; }
    default Iterator<E> call() { return values(); }
}
