package fixture;
public final class Provider {
    public static Object sentinel,context,seenResult;
    public static Callback saved;
    public static Completion savedCompletion;
    public static long clock;
    public static boolean deferred;
    public static <R> Object work(Callback<? super Long,? extends R> callback,Completion<? super R> completion) {
        context=completion.context();
        if(deferred){saved=callback;savedCompletion=completion;return sentinel;}
        R value=callback.apply(Long.valueOf(clock));
        seenResult=value;completion.accept(value);return value;
    }
    public static Object resume() {
        Object value=saved.apply(Long.valueOf(clock));
        seenResult=value;savedCompletion.accept(value);return value;
    }
}
