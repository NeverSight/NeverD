import fixture.Base;
import fixture.Fn;
public final class ProtectedGenericCaller {
    public static Fn<? super Throwable, ? extends Throwable> call(
            Base<Fn<? super Throwable, ? extends Throwable>> receiver, Class<?> type) {
        return receiver.get(type);
    }
}
