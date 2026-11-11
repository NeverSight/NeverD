package fixture;
import java.util.Iterator;
public interface Api<T> extends Root<T> {
    Iterator<T> values();
    default Iterator<T> call() { return values(); }
}
