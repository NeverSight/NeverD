package fixture;
import api.Bound;
public final class Child extends other.Base {
    public Bound run(Bound input) {
        Trace.hit(5);
        Bound result=this.choose(input);
        Trace.after=result;Trace.hit(6);return result;
    }
}
