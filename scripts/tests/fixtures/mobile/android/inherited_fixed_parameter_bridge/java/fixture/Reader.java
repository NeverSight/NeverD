package fixture;
public interface Reader<K> { Result read(K key); Result extra(K key); }
