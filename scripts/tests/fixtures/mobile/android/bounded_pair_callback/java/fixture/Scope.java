package fixture;
public final class Scope<T,V extends Bound> {
    public final Source<T,V> source;
    public final State<T,V> state;
    public final long tick;
    public Scope(Source<T,V> source,State<T,V> state,long tick) {
        this.source=source;this.state=state;this.tick=tick;
    }
}
