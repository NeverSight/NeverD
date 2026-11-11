package fixture;
public abstract class Base<S extends Base<S>> {
    public Object seed,property,snapshot;
    public int stage;
    <K> Base(K seed,Carrier<K> property) {
        Trace.hit(1,this,seed,property,null);
        this.seed=seed;
        this.property=property;
        Trace.hit(2,this,seed,property,null);
        Object result=property.apply(seed);
        snapshot=result;
        Trace.hit(3,this,seed,property,result);
    }
}
