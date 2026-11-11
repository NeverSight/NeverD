package fixture;
public class Base {
    public CharSequence render(Object input) {
        Trace.hit(3); Trace.receiver = this; Trace.inputSeen = input;
        Trace.before.run(); return Trace.output;
    }
    public void apply(CharSequence text, boolean filter) {
        Trace.hit(4); Trace.textSeen = text; Trace.flag = filter;
        Trace.after.run(); Trace.hit(5);
    }
}
