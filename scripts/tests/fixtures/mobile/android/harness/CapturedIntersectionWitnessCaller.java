package probe;
import fixture.*;
public final class CapturedIntersectionWitnessCaller {
    public static void call(Owner receiver, String[] values) { receiver.run(values); }
    public static final class Future extends Owner {
        public <Z extends First & Second> void accept(Z value) { Trace.hit(9); super.accept(value); }
    }
}
