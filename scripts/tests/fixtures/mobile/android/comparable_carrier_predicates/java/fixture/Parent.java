package fixture;
public class Parent<T> {
    public int[] marker;
    public int[] observed;
    public int calls;
    public long seenLeft, seenRight;
    public RuntimeException failure;
    public boolean scalar(long value) {
        calls++; observed = marker; marker[0] += 3; seenLeft = value;
        if (failure != null) throw failure;
        return value >= 0;
    }
    public boolean pair(long left, long right) {
        calls++; observed = marker; marker[0] += 5; seenLeft = left; seenRight = right;
        if (failure != null) throw failure;
        return left < right;
    }
}
