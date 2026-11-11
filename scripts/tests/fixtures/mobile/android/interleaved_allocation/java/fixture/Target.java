package fixture;
public final class Target {
    static {
        Trace.step(1);
        Trace.provider.first=Trace.initFirst;Trace.provider.alternate=Trace.initAlternate;
        Trace.provider.flag=Trace.initFlag;
    }
    public final Object value;
    public final long bits;
    public Target(Object selected,long number) {
        Trace.target=this;Trace.step(4);
        value=selected;Trace.step(5);
        bits=number;Trace.step(6);
    }
}
