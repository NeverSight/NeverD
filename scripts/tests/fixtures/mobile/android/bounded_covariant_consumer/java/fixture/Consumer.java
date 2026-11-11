package fixture;
public class Consumer extends Parent<Payload> {
    public static int events;
    public Carrier<?> savedInput;
    public Object marker;
    public Bound savedValue;
    public RuntimeException failure;
    public final <T extends Bound> void accept(Carrier<? extends T> input) {
        events = events * 5 + 1;
        savedInput = input;
        marker = input.probe.observe(this, input);
        if (failure != null) throw failure;
        savedValue = input.value;
        events = events * 5 + 2;
    }
}
