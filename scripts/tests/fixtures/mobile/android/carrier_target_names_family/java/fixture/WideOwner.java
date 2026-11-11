package fixture;
public class WideOwner implements WideAction {
    public long first, second;
    public int calls;
    public RuntimeException failure;
    public final void apply(long firstValue, long secondValue) {
        calls++;
        first = firstValue;
        if (failure != null) throw failure;
        second = secondValue;
    }
    public static void call(WideAction target, long first, long second) {
        target.apply(first, second);
    }
}
