package fixture;
public class Source<T,V extends Bound> {
    public final T value;
    public final V vector;
    public final long tick;
    public final boolean running;
    public Source(T value,V vector,long tick,boolean running) {
        this.value=value;this.vector=vector;this.tick=tick;this.running=running;
    }
    public T value() { Trace.hit(2);return value; }
    public V vector() { Trace.hit(4);return vector; }
    public long tick() { Trace.hit(7);return tick; }
    public boolean running() { Trace.hit(9);return running; }
}
