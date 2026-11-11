import fixture.*;
public final class StaticLocalCovariantCaller {
    public static Item call(Api api,int index) { return api.get(index); }
}
