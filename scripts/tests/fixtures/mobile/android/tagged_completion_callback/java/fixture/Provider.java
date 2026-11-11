package fixture;
public final class Provider implements Policy {
    public static Object sentinel,context,seenResult;
    public static Callback saved;
    public static Completion savedCompletion;
    public static boolean deferred;
    public long visits;
    public Tag seenTag;
    public <R> Object work(Tag tag,Callback<? super Completion<? super R>,? extends Object> callback,Completion<? super R> completion) {
        seenTag=tag;visits++;context=completion.context();
        if(deferred){saved=callback;savedCompletion=completion;return sentinel;}
        Object value=callback.apply(completion);seenResult=value;return value;
    }
    public static Object resume() {
        Object value=saved.apply(savedCompletion);seenResult=value;return value;
    }
}
