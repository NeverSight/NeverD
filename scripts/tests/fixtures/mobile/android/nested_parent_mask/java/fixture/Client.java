package fixture;
public final class Client {
    public static Core<?> make(int count,long bits,Sink<?> callback,int mask) {
        return new Core<Object>(count,bits,callback,mask,null);
    }
}
