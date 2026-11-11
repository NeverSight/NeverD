package fixture;
import java.util.Iterator;
public final class Core<K,V> extends Middle<K,V> {
    public int calls;
    public RuntimeException failure;
    @SuppressWarnings({"unchecked", "rawtypes"})
    protected Iterator<Pair<? extends K,? extends V>> values(Token<K,V> input) {
        calls++; input.reads++; input.receiver = this;
        if (failure != null) throw failure;
        return (Iterator)input.value;
    }
    public Iterator<?> typed(Token<K,V> input) { return values(input); }
}
