package fixture;
public class ClosedView implements ClosedContract<Object> {
    public java.util.Collection<?> last;
    public int calls;
    public RuntimeException failure;
    public ClosedView() {}
    @Override public boolean addAll(java.util.Collection<?> items) {
        calls++;
        last = items;
        if (failure != null) throw failure;
        return items == null;
    }
}
