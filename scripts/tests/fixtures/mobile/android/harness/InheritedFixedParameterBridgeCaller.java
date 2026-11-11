package fixture;
public final class InheritedFixedParameterBridgeCaller {
    public static <K> Result generic(Reader<K> receiver,K key){return receiver.extra(key);}
}
