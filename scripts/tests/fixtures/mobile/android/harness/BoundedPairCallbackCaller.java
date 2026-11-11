import fixture.*;
public final class BoundedPairCallbackCaller {
    @SuppressWarnings("unchecked")
    public static Object call(Source source,State state,long tick,Action action,Completion completion) {
        return Provider.consume(source,state,tick,action,completion);
    }
}
