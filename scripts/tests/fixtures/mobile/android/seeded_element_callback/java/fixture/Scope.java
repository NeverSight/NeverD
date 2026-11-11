package fixture;
public final class Scope implements Context {
    public static final class Helper {
        public static <T> T fold(Scope scope,T seed,Fn<? super T,? super Context,? extends T> fn) {
            return Provider.fold((Context)scope,seed,fn);
        }
    }
}
