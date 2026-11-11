package fixture;
public class Child extends Worker {
    public <T> void send(Factory<? extends T> factory) {
        Trace.hit(4);
        Trace.hit(1);
        Object value = factory.make();
        calls++;
        last = value;
        if (value != null) ((int[])value)[0]++;
        Trace.hit(3);
        Trace.hit(5);
    }
}
