package fixture;
public final class ClosedClassInterfaceBridgeCaller {
    public static <K> Result generic(Reader<K> receiver,K key){return receiver.read(key);}
}
