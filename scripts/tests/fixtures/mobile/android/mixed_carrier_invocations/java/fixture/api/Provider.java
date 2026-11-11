package fixture.api;
import fixture.*;
class Provider {
    static { Trace.hit(1); }
    public static <T> T make(Object[] inputs, Saver<T, ? extends Object> saver, Factory<? extends T> producer, long flags) {
        Trace.flags = flags; Trace.hit(2);
        Trace.inputs = inputs; Trace.key = inputs[0];
        T result = producer.read();
        Trace.saved = saver.save(result); Trace.hit(6);
        return result;
    }
}
