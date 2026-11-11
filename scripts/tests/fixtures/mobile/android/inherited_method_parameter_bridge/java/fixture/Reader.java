package fixture;
public interface Reader<K> { Result read(K key); <E extends Enum<E>> E extra(Class<E> token,K key); }
