package fixture;
public final class AnonymousClosedParentBridgeCaller {
    public static <T> T generic(Reader<T> receiver,float fraction,T left,T right) {
        return receiver.read(fraction,left,right);
    }
}
