import fixture.*;
public final class GenericDefaultFoldCaller {
    public static Object call(Api receiver, Object seed, Bi<Object,Api,Object> operation) {
        return Api.DefaultImpls.fold(receiver, seed, operation);
    }
}
