package fixture;
public class Parent<T> {
    public int calls;
    public int[] marker, observed;
    public RuntimeException failure;
    public boolean scalar(double value) {
        calls++;
        observed = marker;
        marker[0] += 3;
        if (failure != null) throw failure;
        return value >= 'a' && value <= 'z';
    }
    public boolean pair(double left, double right) {
        calls++;
        observed = marker;
        marker[0] += 3;
        if (failure != null) throw failure;
        return left <= right;
    }
}
