package fixture;
public interface View<K> extends Source<K> {
    default <E extends Enum<E>> E select(Class<E> type,K key){return select(type,key,null);}
}
