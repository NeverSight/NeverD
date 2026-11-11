package fixture;

public interface Contract<K> {
    <M extends Enum<M>> M pick(Class<M> kind, K key);
}
