package fixture;
public class Base {
    public <T extends First & Second> void accept(T value) {
        Trace.hit(2);
        Trace.receiver = this;
        Trace.value = value;
        Trace.before.run();
        Trace.hit(3);
        if (value != null) value.touch();
        Trace.hit(5);
    }
}
