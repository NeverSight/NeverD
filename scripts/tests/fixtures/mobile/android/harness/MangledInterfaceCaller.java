import fixture.*;
public final class MangledInterfaceCaller {
    public static Object call(Api<Object> api, Object value) { return NamesKt.call(api, value); }
    public static Object inherited(Branch<Object> api, Object value) { return NamesKt.inherited(api, value); }
    public static Object concrete(Relay<Object> api, Object value) { return NamesKt.concrete(api, value); }
}
