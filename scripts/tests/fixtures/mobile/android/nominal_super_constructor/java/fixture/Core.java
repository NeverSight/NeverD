package fixture;
public final class Core extends Parent<Token> {
    public final Token saved;
    public final long childStamp;
    public Core(Token value,long stamp) {
        super(value,stamp);
        Trace.step(3);
        saved=value;childStamp=stamp;
        Trace.saved=value;Trace.stampSeen=stamp;
        Trace.step(4);
        Trace.corePartial=this;
    }
}
