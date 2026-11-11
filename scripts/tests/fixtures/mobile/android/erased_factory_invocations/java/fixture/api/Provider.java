package fixture.api;
import fixture.*;
class Provider {
    static { Trace.hit(1); }
    public static <T> Box<T> make(Factory<? extends T> factory, long flags) {
        Trace.flags = flags; Trace.hit(2);
        T value = factory.read();
        return new Box<T>(value);
    }
    public static <T> T element(Factory<? extends T> producer, long flags) {
        Trace.flags = flags; Trace.hit(2);
        T result = producer.read();
        Trace.hit(6); return result;
    }
    public static <T> Box<T> seed(T value, long flags) {
        Trace.flags = flags; Trace.hit(2);
        return new Box<T>(value);
    }
}
