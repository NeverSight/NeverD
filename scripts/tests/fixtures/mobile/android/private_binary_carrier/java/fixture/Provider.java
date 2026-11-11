package fixture;
@SuppressWarnings({"rawtypes","unchecked"})
public class Provider {
    static { Trace.hit(1); }
    public Provider() {}
    private final <T> Carrier<T> read(Carrier<? extends T> left,Carrier<? extends T> right) {
        Trace.receiver=this;Trace.left=left;Trace.right=right;Trace.calls++;Trace.hit(2);
        Object first=left==null?null:left.get();Trace.first=first;Trace.hit(3);
        Object second=right==null?null:right.get();Trace.second=second;Trace.hit(4);
        Carrier<T> result=(Carrier<T>)Trace.result;Trace.hit(5);
        return result;
    }
    public static Carrier call(Provider peer,Carrier left,Carrier right) {
        return peer.read(left,right);
    }
}
