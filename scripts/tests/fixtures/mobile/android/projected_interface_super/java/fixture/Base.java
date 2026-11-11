package fixture;
public class Base {
    public final int label;
    public final Sink<Object> sink;
    public Base(int label,Sink<Object> sink) {
        Trace.partial=this;Trace.hit(1);sink.accept(this);this.sink=sink;this.label=label;Trace.hit(3);
    }
}
