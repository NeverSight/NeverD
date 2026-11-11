package fixture;
import java.util.concurrent.atomic.AtomicReference;
import kotlin.jvm.functions.Function1;
import kotlin.jvm.functions.Function2;
import kotlin.coroutines.Continuation;
public final class Carrier<T> {
    private final AtomicReference<Slot<T>> value;
    private Carrier(AtomicReference value) { this.value=value; }
    public static final Carrier wrap(AtomicReference value) { return new Carrier(value); }
    public final AtomicReference unwrap() { return value; }
    private static final class Slot<T> {
        private final T value;
        private Slot(T value) { this.value=value; }
        public T value() { return value; }
    }
    public static <R> Object process(AtomicReference<Slot<Object>> state,
            Function1<? super Context,? extends Object> factory,
            Function2<? super Object,? super Continuation<? super R>,? extends Object> action,
            Continuation<? super R> frame) {
        Trace.step(1,state,frame);
        Object element=factory.invoke(Trace.context);
        Trace.produced=element;
        Trace.step(2,state,frame);
        Object result=action.invoke(element,frame);
        Trace.returned=result;
        Trace.step(3,state,frame);
        return result;
    }
}
