package fixture;
public class Provider {
    public <T extends Bound & Flag> void accept(long number, int label, T value,
                                               boolean enabled, double fraction) {
        Trace.hit(4);
        Trace.receiver = this;
        Trace.value = value;
        Trace.number = number;
        Trace.label = label;
        Trace.enabled = enabled;
        Trace.fraction = fraction;
        Trace.hit(5);
        Trace.action.run();
        Trace.hit(6);
        if (value != null) value.touch();
        Trace.hit(8);
    }
}
