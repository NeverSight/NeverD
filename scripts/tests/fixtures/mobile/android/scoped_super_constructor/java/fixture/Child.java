package fixture;
public final class Child extends Base<Child> {
    public <V> Child(V seed,Carrier<V> property) {
        super(seed,property);
        stage=1;
        Trace.hit(4,this,seed,property,snapshot);
    }
}
