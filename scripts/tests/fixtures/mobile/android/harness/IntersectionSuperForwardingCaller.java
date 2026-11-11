package probe;
import fixture.*;
public final class IntersectionSuperForwardingCaller {
    public static void call(Owner receiver, Value value) { receiver.accept(value); }
    public static class Value implements First, Second {
        public void touch() { Trace.hit(4); }
    }
    public static final class Future extends Owner {
        public <Z extends First & Second> void accept(Z value) {
            Trace.hit(1); super.accept(value); Trace.hit(8);
        }
    }
}
