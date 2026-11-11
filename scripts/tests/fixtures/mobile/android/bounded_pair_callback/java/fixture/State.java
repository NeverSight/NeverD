package fixture;
public final class State<T,V extends Bound> {
    public T value;
    public V vector;
    public long tick;
    public boolean running;
    public void value(T item) { Trace.hit(3);value=item; }
    public void vector(V item) { Trace.hit(5);vector=item;Trace.hit(6); }
    public void tick(long item) { Trace.hit(8);tick=item; }
    public void running(boolean item) { Trace.hit(10);running=item; }
}
