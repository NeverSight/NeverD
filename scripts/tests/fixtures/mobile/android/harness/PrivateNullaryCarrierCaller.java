package fixture;
public final class PrivateNullaryCarrierCaller {
    public static Object call(boolean checked) {return checked?Owner.text():Owner.call();}
}
