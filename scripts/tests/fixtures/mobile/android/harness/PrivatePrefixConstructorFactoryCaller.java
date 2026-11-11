package probe;
import fixture.*;
public final class PrivatePrefixConstructorFactoryCaller {
    public static Child call(boolean enabled) { return new Child(enabled); }
    public static final class Future extends Value {
        public void configure(float amount) { Trace.hit(9); super.configure(amount); Trace.hit(8); }
    }
}
