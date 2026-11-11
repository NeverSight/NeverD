package fixture;
public final class Core implements Producer {
    static { Trace.hit(1); }
    public final Value get() {
        Trace.hit(2);
        Value result=Trace.value;
        Trace.current=result; Trace.hit(3);
        Trace.value=Trace.replacement; Trace.hit(4);
        return result;
    }
}
