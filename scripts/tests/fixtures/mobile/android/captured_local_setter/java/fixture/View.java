package fixture;
public interface View<K,V> extends java.util.Map.Entry<K,V> {
    V peek();
    void restore(V input);
}
