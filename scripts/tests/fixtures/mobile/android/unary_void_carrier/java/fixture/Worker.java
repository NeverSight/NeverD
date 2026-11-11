package fixture;
public class Worker implements Provider {
    public Object last;
    public int calls;
    public <T> void send(Factory<? extends T> factory) {
        Trace.hit(1);
        Object value = factory.make();
        calls++;
        last = value;
        if (value != null) ((int[])value)[0]++;
        Trace.hit(3);
    }
}
