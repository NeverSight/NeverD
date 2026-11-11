package probe;
import fixture.*;
public final class BranchedInterfaceSuperAccessorCaller {
    public static int call(Child receiver, long value) {
        Trace.hit(6);
        int result = Child.DefaultImpls.measure(receiver, value);
        Trace.hit(0);
        return result;
    }
    public static class Future extends Receiver {
        @Override public int read(long value) {
            Trace.hit(7);
            int result = super.read(value);
            Trace.hit(8);
            return result;
        }
        @Override public int measure(long value) {
            Trace.overrides++;
            Trace.hit(9);
            return ~Trace.other;
        }
    }
}
