package fixture;
public final class Core {
    public final Policy policy;
    public final Object first,second;
    public Core() { this((Policy)Pool.value);Trace.step(8); }
    public Core(int hint) { this(Factory.make(hint));Trace.step(8); }
    public Core(Policy policy) {
        Trace.escaped=this;Trace.step(3);
        this.policy=policy;Trace.step(4);
        this.first=Trace.produce(5);
        this.second=Trace.produce(6);
        Trace.step(7);
    }
}
