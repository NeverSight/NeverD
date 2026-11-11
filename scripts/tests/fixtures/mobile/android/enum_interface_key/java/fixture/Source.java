package fixture;
public interface Source<K> {<E extends Enum<E>> E select(Class<E> type,K key,E fallback);}
