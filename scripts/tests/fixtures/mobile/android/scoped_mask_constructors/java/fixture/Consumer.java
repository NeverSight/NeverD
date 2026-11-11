package fixture;
public class Consumer implements Sink<Object> {
    public Consumer() { Trace.hit(3); }
    public void accept(Object value) { Trace.seen = value; Trace.hit(5); }
}
