package fixture;
import java.util.Iterator;
public interface Api<T> {
    NarrowIterator<T> values();
    default Iterator<T> call() { return values(); }
}
