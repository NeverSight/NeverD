package fixture;
public class Core<G> implements Narrow<G> {
    public int calls;
    public Object value, seenInput;
    public Class<?> seenClass;
    public First failure;
    public Second second;
    public G read(G input) throws First, Second {
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
