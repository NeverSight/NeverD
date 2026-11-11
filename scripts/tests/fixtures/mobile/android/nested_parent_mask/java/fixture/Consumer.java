package fixture;
public final class Consumer implements Sink<Object> {
    public void accept(Object value) { Trace.callback=this;Trace.seen=value;Trace.hit(8); }
}
