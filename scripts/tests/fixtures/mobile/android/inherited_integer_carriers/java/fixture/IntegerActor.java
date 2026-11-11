package fixture;
@SuppressWarnings("rawtypes")
public final class IntegerActor implements IntegerFunction {
    public Object selected;
    public IntegerTag last;
    public int calls;
    public int mask;
    public RuntimeException failure;
    public Object invoke(IntegerTag first, int second) {
        calls++; last = first; mask = second;
        if (failure != null) throw failure;
        return selected;
    }
    public Object invoke(Object first, Object second) {
        return invoke((IntegerTag) first, ((Number) second).intValue());
    }
}
