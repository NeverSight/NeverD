package fixture;
public class Core extends Middle {
    public Object read() { Trace.hit(9); return Trace.replacement; }
    public int number() { Trace.hit(9); return Trace.afterNumber; }
    public Target make() {
        Target result = new Target(this, super.read(), super.number());
        Trace.after = result.value;
        Trace.afterInt = result.number;
        Trace.hit(8);
        return result;
    }
    public Target live() {
        Object saved;
        int number;
        Target result = new Target(this, saved = super.read(), number = super.number());
        Trace.after = saved;
        Trace.afterInt = number;
        Trace.hit(8);
        return result;
    }
}
