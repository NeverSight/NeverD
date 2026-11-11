package fixture;

public class Reader<T> implements Refined<T> {
    public int calls;
    public Class<?> type;
    public T seen;
    public RuntimeException failure;

    public Reader() {}

    public void touch(Class<?> type, T key) {
        calls++;
        this.type = type;
        seen = key;
        if (failure != null) throw failure;
    }
}
