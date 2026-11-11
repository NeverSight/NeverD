package fixture;
public class Provider<T> implements Api<T> {
    public Carrier<T, Provider<T>> value;
    public int calls;
    public RuntimeException failure;
    public Carrier<T, Provider<T>> result() {
        calls++;
        if (failure != null) throw failure;
        return value;
    }
}
