package fixture;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Receiver<S> extends Base implements Function {
    public Receiver(Sink<? super Receiver> sink) { super(2,(Sink<Object>)(Sink)sink);Trace.hit(4); }
    public Object invoke(Object a,Object b) { return CarrierB.wrap(apply(((CarrierA)a).unwrap(),((CarrierB)b).unwrap())); }
    public final long apply(long a,long b) { Trace.hit(6);return b; }
}
