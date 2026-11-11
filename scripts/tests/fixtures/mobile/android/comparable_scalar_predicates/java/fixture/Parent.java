package fixture;
public class Parent<T> {
    public int calls;
    public int[] marker, observed;
    public RuntimeException failure;
    public boolean scalar(char value) {
        calls++;
        observed = marker;
        marker[0] += 3;
        if (failure != null) throw failure;
        return value >= 'a' && value <= 'z';
    }
}
