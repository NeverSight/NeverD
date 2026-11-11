package fixture;
import java.util.Iterator;
public interface Contract<E> {
    Iterator<E> values();
}
