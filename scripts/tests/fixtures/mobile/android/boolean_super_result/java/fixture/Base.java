package fixture;
public class Base<K,V> {
    public boolean change(K key,V expected,V update) {
        Trace.hit(2);Trace.receiver=this;Trace.key=key;Trace.expected=expected;Trace.update=update;
        Trace.action.run();Trace.hit(3);return Trace.result;
    }
}
