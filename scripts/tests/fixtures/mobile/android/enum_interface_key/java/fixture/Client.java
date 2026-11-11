package fixture;
public class Client {
    public static <K,E extends Enum<E>> E call(View<K> view,Class<E> type,K key){return view.select(type,key);}
    public static <K,E extends Enum<E>> E fallback(Source<K> source,Class<E> type,K key,E fallback){return source.select(type,key,fallback);}
}
