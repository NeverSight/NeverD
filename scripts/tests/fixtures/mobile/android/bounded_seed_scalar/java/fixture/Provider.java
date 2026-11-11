package fixture;
public final class Provider {
    public static Result output;
    private static <V extends Bound> Result make(V seed,float first,float second) {
        Trace.seed=seed;Trace.first=first;Trace.second=second;Trace.hit(1);
        if(seed==null) { Trace.hit(2);return null; }
        Trace.hit(3);
        Result result=output;
        Trace.result=result;
        Trace.hit(4);
        return result;
    }
    public static Result access(Bound seed,float first,float second) { return make(seed,first,second); }
}
