package fixture;
public class Provider {
    public <T extends First & Second> void accept(T value) {
        Trace.hit(5); Trace.receiver = this; Trace.argument = value;
        Trace.before.run(); value.touch(); Trace.hit(7);
    }
}
