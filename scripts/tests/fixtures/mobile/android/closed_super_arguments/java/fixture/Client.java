package fixture;
public final class Client {
    public static Child build(Provider provider,Fn<Object,Unit> first,Fn<Object,Unit> second,Object token) {
        return new Child(provider,first,second,token);
    }
    public static Unit call(Parent value,Object input,boolean write) { return value.call(input,write); }
}
