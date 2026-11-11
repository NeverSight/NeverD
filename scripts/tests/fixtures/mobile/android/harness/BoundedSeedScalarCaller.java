import fixture.*;
public final class BoundedSeedScalarCaller {
    public static Result call(Bound seed,float first,float second) { return Provider.access(seed,first,second); }
}
