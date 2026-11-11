package fixture;
public final class InheritedMethodParameterBridgeCaller {
    public static <K,E extends Enum<E>> E generic(Reader<K> receiver,Class<E> token,K key){return receiver.extra(token,key);}
}
