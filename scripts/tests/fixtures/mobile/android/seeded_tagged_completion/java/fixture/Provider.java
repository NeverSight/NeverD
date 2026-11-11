package fixture;
public final class Provider implements Policy {
    public static Object sentinel,context,seenResult;
    public static Callback saved;
    public static Object savedInput,seenInput;
    public static Completion savedCompletion;
    public static boolean deferred;
    public long visits;
    public Tag seenTag;
    public <T,R> Object work(T input,Tag tag,Callback<? super T,? super Completion<? super R>,? extends Object> callback,Completion<? super R> completion) {
        seenInput=input;seenTag=tag;visits++;context=completion.context();
        if(deferred){saved=callback;savedInput=input;savedCompletion=completion;return sentinel;}
        Object value=callback.apply(input,completion);seenResult=value;return value;
    }
    public static Object resume() {
        Object value=saved.apply(savedInput,savedCompletion);seenResult=value;return value;
    }
}
