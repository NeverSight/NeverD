package fixture;
import kotlin.Unit;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Receiver<K,V> implements Function {
    public boolean flag;
    public K key;
    public V previous;
    public V replacement;
    public RuntimeException failure;
    public Receiver() {}
    public final void invoke(boolean flag, K key, V previous, V replacement) {
        Trace.events = Trace.events * 10 + 1;
        this.flag = flag;
        this.key = key;
        this.previous = previous;
        this.replacement = replacement;
        if (failure != null) throw failure;
    }
    public Object invoke(Object flag, Object key, Object previous, Object replacement) {
        invoke(((Boolean) flag).booleanValue(), (K) key, (V) previous, (V) replacement);
        return Unit.INSTANCE;
    }
}
