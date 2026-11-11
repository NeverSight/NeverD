package fixture;
public final class Client {
    public static Object run(Scope scope,Object seed,Fn fn) { return Scope.Helper.fold(scope,seed,fn); }
}
