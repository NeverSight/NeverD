package fixture;
public interface Contract<K, V> {
    default Contract<K, V> listener(Callback<K, V> callback) {
        Trace.defaultCalls++;
        Trace.events = Trace.events * 10 + 9;
        if (Trace.defaultFailure != null) throw Trace.defaultFailure;
        return this;
    }
}
