package fixture;
import java.util.Set;
public class Counter<T> implements CountView<T> {
    public Set<T> values;
    public Set<T> last;
    public int calls;
    public RuntimeException failure;
    public Counter() {}
    public Set<T> getItems() {
        calls++;
        last = values;
        if (failure != null) throw failure;
        return values;
    }
    public final Set<T> items() {
        return getItems();
    }
}
