package fixture;
public final class Child extends Base<Child> {
    public <V> Child(V seed,Carrier<V> property,float number) {
        super(seed,property);
        auxiliary=number;
        stage=1;
        Trace.hit(4,this,seed,property,snapshot);
    }
}
