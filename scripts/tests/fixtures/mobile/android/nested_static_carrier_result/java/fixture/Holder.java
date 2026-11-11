package fixture;
import java.util.concurrent.atomic.AtomicReference;
public final class Holder<T> {
    private Holder() {}
    private static final class Slot<T> {
        private final T value;
        private Slot(T value) { this.value=value; }
        public T value() { return value; }
    }
    public static <T> AtomicReference<Slot<T>> build() {
        Trace.hit(1,null);
        AtomicReference<Slot<T>> value=new AtomicReference<Slot<T>>(null);
        return same(value);
    }
    private static <T> AtomicReference<Slot<T>> same(AtomicReference<Slot<T>> value) {
        Trace.hit(2,value);
        return value;
    }
    public static Object send() { return build(); }
}
