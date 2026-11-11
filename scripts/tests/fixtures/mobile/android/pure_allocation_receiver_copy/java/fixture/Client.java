package fixture;
public final class Client {
    public static Target make(long bits,boolean enabled,int tag,boolean active) { return new Target(bits,enabled,tag,active); }
}
