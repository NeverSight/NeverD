import fixture.*;
public final class ScalarIntersectionArgumentFuture extends Child {
    @Override public <T extends Bound & Flag> void accept(long number, int label, T value,
                                                         boolean enabled, double fraction) {
        Trace.hit(1);
        super.accept(number, label, value, enabled, fraction);
        Trace.hit(9);
    }
}
