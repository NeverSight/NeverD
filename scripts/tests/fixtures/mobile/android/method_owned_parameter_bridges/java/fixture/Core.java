package fixture;

public class Core implements Contract<String> {
    public <N extends Enum<N>> N pick(Class<N> kind, String key) {
        Trace.calls++;
        Trace.kind = kind;
        Trace.key = key;
        if (Trace.failure != null) throw Trace.failure;
        return kind.cast(Trace.value);
    }
}
