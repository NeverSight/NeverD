package fixture;
public final class Provider {
    public static Object sentinel,context,seenResult;
    public static Callback saved;
    public static Completion savedCompletion;
    public static Scope scope,savedScope,seenScope;
    public static boolean deferred;
    public static <R> Object work(Callback<? super Scope,? super Completion<? super R>,? extends Object> callback,Completion<? super R> completion) {
        context=completion.context();seenScope=scope;
        if(deferred){saved=callback;savedScope=scope;savedCompletion=completion;return sentinel;}
        Object value=callback.apply(scope,completion);seenResult=value;return value;
    }
    public static Object resume(){Object value=saved.apply(savedScope,savedCompletion);seenResult=value;return value;}
}
