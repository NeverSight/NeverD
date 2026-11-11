import fixture.*;
public class InstanceIntersectionDerived extends Child {
    public <T extends First & Second> void accept(T value) {
        Trace.receiver = this; Trace.argument = value;
        Trace.hit(7);
        Trace.hit(2);
        value.first();
        Trace.hit(4);
        value.second();
        Trace.hit(6);
    }
}
