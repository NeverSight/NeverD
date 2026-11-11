package fixture;
import java.util.Iterator;
public final class View implements Contract<Pair<? super Object,? super Object>> {
    public Iterator<Pair<Object,Object>> value;
    public int calls;
    public Object marker, observed;
    public RuntimeException failure;
    public View() {}
    @SuppressWarnings({"unchecked", "rawtypes"})
    public Iterator<Pair<? super Object,? super Object>> values() {
        calls++;
        observed = marker;
        if (marker instanceof int[]) ((int[]) marker)[0] += 3;
        if (failure != null) throw failure;
        return (Iterator) value;
    }
    public Iterator<Pair<? super Object,? super Object>> entry() { return values(); }
}
