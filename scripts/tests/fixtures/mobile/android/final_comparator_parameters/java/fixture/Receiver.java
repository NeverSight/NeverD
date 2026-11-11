package fixture;
import java.util.Comparator;
import java.util.List;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Receiver<S> implements Comparator {
    public Pair<Value, ? extends List<Node>> first, second;
    public int answer, events;
    public RuntimeException failure;
    public int compare(Object first, Object second) {
        return compare((Pair) first, (Pair) second);
    }
    public final int compare(Pair<Value, ? extends List<Node>> first,
                             Pair<Value, ? extends List<Node>> second) {
        events++;
        this.first = first;
        this.second = second;
        if (failure != null) throw failure;
        return answer;
    }
}
