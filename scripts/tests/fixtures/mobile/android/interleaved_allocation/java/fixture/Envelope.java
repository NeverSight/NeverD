package fixture;
public final class Envelope {
    static { Trace.step(7); }
    public final Target child;
    public Envelope(Target value) {
        Trace.outer=this;Trace.step(8);child=value;Trace.step(9);
    }
}
