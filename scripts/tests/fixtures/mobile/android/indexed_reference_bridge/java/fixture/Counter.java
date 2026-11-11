package fixture;
import java.util.List;
public class Counter<T> implements CountView<T> {
    public List<T> values;
    public int calls;
    public int lastIndex;
    public RuntimeException failure;
    public Counter() {}
    public T removeAt(int index) {
        calls++;
        lastIndex = index;
        if (failure != null) throw failure;
        return values.remove(index);
    }
    public final T remove(int index) { return removeAt(index); }
}
