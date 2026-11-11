package fixture;
import java.util.Iterator;
public interface CountView<E> {
    boolean add(E value);
    Iterator<E> iterator();
}
