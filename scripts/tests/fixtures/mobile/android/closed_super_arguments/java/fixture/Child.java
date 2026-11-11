package fixture;
public final class Child extends Parent {
    public final Object marker;
    public Child(Provider provider,Fn<Object,Unit> first,Fn<Object,Unit> second,Object token) {
        long bits=Trace.wide();
        Fn<Object,Unit> read=first!=null ? first : provider!=null ? provider.read() : Trace.read();
        Fn<Object,Unit> write=second!=null ? second : provider!=null ? provider.write() : Trace.write();
        super(bits,token,read,write);
        Trace.hit(7);
        marker=token;
    }
}
