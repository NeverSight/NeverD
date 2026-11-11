package fixture;

public class Core<T extends Tag, U extends Tag> implements Contract<T, U> {
    public int calls;
    public T last;
    public RuntimeException failure;

    public Core() {}

    public T echo(T value) {
        calls++;
        last = value;
        if (failure != null) throw failure;
        return value;
    }

    public T echo(T value, int delta) {
        calls += delta;
        last = value;
        if (failure != null) throw failure;
        return value;
    }

    public T[] array(T[] values) {
        calls++;
        if (failure != null) throw failure;
        last = values[0];
        return values;
    }

    public T[] newArray(int size) {
        calls += size;
        if (failure != null) throw failure;
        return null;
    }
}
