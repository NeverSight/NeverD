package fixture;
public final class Maker<T> {
    public static Object observed(Object input) {
        Trace observer = Trace.observer;
        Object result;
        try {
            result = new Payload(switch (0) { default -> { Trace.prepare(); yield input; } });
        } catch (Throwable failure) {
            Trace.logFailure("owned first", "owned second", failure);
            observer.seen = failure;
            observer.context = input;
            observer.code = 0;
            result = failure;
        }
        observer.checked = true;
        return result;
    }
    public static Object fallback() {
        Trace observer = Trace.observer;
        Object result;
        try {
            result = new Payload(switch (0) { default -> { Trace.prepare(); yield Trace.token; } });
        } catch (Throwable failure) {
            Trace.logFailure("owned first", "owned second", failure);
            observer.seen = failure;
            observer.context = Trace.token;
            observer.code = 0;
            result = Fallback.value;
        }
        observer.checked = true;
        return result;
    }
}
