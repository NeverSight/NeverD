import fixture.*;
public final class PlainNonReturningVoidCaller {
    public static void dispatch(Base value, Object first, Object second) { value.dispatch(first, second); }
    public static void schedule(Api value, long delay, Entry<? super Throwable> entry) { value.schedule(delay, entry); }
}
