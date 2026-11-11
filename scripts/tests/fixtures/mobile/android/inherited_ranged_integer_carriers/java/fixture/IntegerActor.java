package fixture;
@SuppressWarnings("rawtypes")
public final class IntegerActor implements IntegerFunction {
    public Object selected;
    public Object[] captured;
    public IntegerTag last;
    public int calls, firstMask, secondMask;
    public RuntimeException failure;
    public Object invoke(Object p0, Object p1, Object p2, Object p3, Object p4, Object p5, Object p6, Object p7, Object p8, Object p9, IntegerTag tag, int first, int second) {
        calls++; last = tag; firstMask = first; secondMask = second;
        captured = new Object[] {p0, p1, p2, p3, p4, p5, p6, p7, p8, p9};
        if (failure != null) throw failure;
        return selected;
    }
    public Object invoke(Object p0, Object p1, Object p2, Object p3, Object p4, Object p5, Object p6, Object p7, Object p8, Object p9, Object tag, Object first, Object second) {
        return invoke(p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, (IntegerTag) tag, ((Number) first).intValue(), ((Number) second).intValue());
    }
}
