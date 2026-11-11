package fixture;
public class Value implements Input {
    public int read() {
        Trace.hit(5);
        Trace.valueReceiver = this;
        Trace.valueAction.run();
        Trace.hit(6);
        return Trace.answer;
    }
}
