package fixture;
public abstract class Base<A,B> implements Sink<B> {
    public final A source;
    public final B target;
    public final Config config;
    public Base(A source, B target, Config config) {
        this.source = source;
        this.target = target;
        this.config = config;
        Trace.bases++;
        Trace.source = source;
        Trace.target = target;
        Trace.config = config;
        Trace.stage = 1;
        Trace.tag = config.tag;
        Trace.stage = 2;
        config.action.run();
        Trace.stage = 3;
    }
}
