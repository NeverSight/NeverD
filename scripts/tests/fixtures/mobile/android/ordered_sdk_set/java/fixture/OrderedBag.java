package fixture;
import java.util.Comparator;
import java.util.TreeSet;
public final class OrderedBag<E> extends TreeSet<E> {
    public OrderedBag(Comparator<? super E> comparator) { super(comparator); }
    public int getSize() { return super.size(); }
    @Override public final int size() { return getSize(); }
}
