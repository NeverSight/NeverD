import fixture.*;

public class InheritedCovariantSlotDerived extends Worker {
    @Override public Base update(int code, Object[] values, long wide, double real) {
        Trace.derivedCalls++;
        return super.update(code, values, wide, real);
    }
}
