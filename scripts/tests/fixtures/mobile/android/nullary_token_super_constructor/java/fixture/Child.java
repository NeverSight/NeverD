package fixture;
public class Child extends Parent<Marker, Probe> {
    public Object value;
    private Child() {
        super(Probe.class, "owned token label");
        Trace.hit(5); value = Trace.result; Trace.hit(6);
    }
    public static Child create() { return new Child(); }
}
