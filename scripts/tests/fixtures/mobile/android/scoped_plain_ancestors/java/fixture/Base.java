package fixture;
public class Base implements Api {
    public int calls;
    public Object value, seenInput;
    public Class<?> seenClass;
    public First failure;
    public Second second;
    public Object read(Object input) throws First, Second {
        calls++;
        seenInput = input;
        seenClass = null;
        if (failure != null) throw failure;
        if (second != null) throw second;
        return input;
    }
    public <R> R unwrap(Class<R> kind) throws First {
        calls++;
        seenClass = kind;
        seenInput = value;
        if (failure != null) throw failure;
        return kind.cast(value);
    }
}
