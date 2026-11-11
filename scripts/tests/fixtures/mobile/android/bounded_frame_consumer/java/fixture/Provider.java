package fixture;
public final class Provider {
    public static Object sentinel,context,seenResult;
    public static Callback saved;
    public static Completion savedCompletion;
    public static long clock;
    public static boolean deferred;public static Carrier seenCarrier;public static boolean seenInfinite;
    private static <R,T,V extends Bound> Object work(Carrier<T,V> carrier,Callback<? super Long,? extends R> callback,Completion<? super R> completion) {
        seenCarrier=carrier;seenInfinite=carrier.infinite();context=completion.context();
        if(deferred){saved=callback;savedCompletion=completion;return sentinel;}
        R value=callback.apply(Long.valueOf(clock));
        seenResult=value;completion.accept(value);return value;
    }
    public static Object access(Carrier carrier,Callback callback,Completion completion){return work(carrier,callback,completion);}
    public static Object resume() {
        Object value=saved.apply(Long.valueOf(clock));
        seenResult=value;savedCompletion.accept(value);return value;
    }
}
