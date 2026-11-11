package probe;
import fixture.*;
public final class ClosedVisibilityResultBridgeCaller {
    public static Input input(Child child) {
        Trace.hit(1);
        Input result = child.first();
        Trace.hit(8);
        return result;
    }
    public static Value value(Child child) {
        Trace.hit(1);
        Value result = child.first();
        Trace.hit(8);
        return result;
    }
    public static final class Future extends Value {
        @Override public int read() {
            Trace.hit(7);
            int result = super.read();
            Trace.hit(9);
            return result;
        }
    }
}
