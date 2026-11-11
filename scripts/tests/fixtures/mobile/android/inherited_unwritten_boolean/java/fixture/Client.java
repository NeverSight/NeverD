package fixture;
public final class Client {
    public static Child create(long value,int marker,boolean delegate) {
        if(delegate)return new Child(value,marker);
        return new Child(value);
    }
}
