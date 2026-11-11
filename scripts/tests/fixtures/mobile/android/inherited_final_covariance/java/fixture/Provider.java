package fixture;
public class Provider<T, U> {
    public Narrow<T> value;
    public Narrow<T> last;
    public int calls;
    public RuntimeException failure;
    public final Narrow<T> get() {
        calls++;
        last = value;
        if (failure != null) throw failure;
        return value;
    }
}
