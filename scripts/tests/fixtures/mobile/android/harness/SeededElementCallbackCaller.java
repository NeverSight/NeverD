import fixture.*;
public final class SeededElementCallbackCaller {
    public static Object run(Scope scope,Object seed,Fn fn) { return Scope.Helper.fold(scope,seed,fn); }
}
