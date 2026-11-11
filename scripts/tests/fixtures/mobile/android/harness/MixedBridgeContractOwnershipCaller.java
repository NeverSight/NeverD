package fixture;
public final class MixedBridgeContractOwnershipCaller {
    public static <K> Result generic(Reader<K> receiver,K key){return receiver.read(key);}
}
