import fixture.*;
public final class PrivateBinaryCarrierCaller {
    public static Carrier call(Provider peer,Carrier left,Carrier right) { return Provider.call(peer,left,right); }
    public static String element(Carrier value) { return (String)value.get(); }
}
