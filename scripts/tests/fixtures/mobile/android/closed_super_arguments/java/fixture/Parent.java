package fixture;
public class Parent {
    public final long bits;
    public final Object token;
    public final Fn<Object,Unit> first,second;
    public Parent(long bits,Object token,Fn<Object,Unit> first,Fn<Object,Unit> second) {
        Trace.hit(6);
        this.bits=bits;this.token=token;this.first=first;this.second=second;
        Trace.partial=this;
    }
    public Unit call(Object input,boolean write) {
        Trace.hit(8);
        return (write ? second : first).apply(input);
    }
}
