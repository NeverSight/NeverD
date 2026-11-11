package fixture;
public final class Provider {
    public static <T> T fold(Context context,T seed,Fn<? super T,? super Context,? extends T> fn) {
        Trace.seenContext=context;Trace.seenSeed=seed;Trace.seenFn=fn;Trace.hit(1);
        T first=fn.apply(seed,context);Trace.first=first;Trace.hit(3);
        T second=fn.apply(first,context);Trace.second=second;Trace.hit(5);
        return Trace.override?(T)Trace.result:second;
    }
}
