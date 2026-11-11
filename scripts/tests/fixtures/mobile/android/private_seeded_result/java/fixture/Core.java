package fixture;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private final <R,S> R read(S seed,Callback<? super S,Token> callback,Factory<? extends R> producer) {
        Trace.seenReceiver=this; Trace.seenSeed=seed; Trace.hit(1);
        Token token=callback.apply(seed); Trace.token=token; Trace.hit(2);
        R result=producer.get(); Trace.hit(3);
        Trace.saved=result; Trace.hit(4);
        return result;
    }
    public static Value checked(Core peer,Object seed,Callback callback,Factory producer) {
        Trace.hit(5); Object result=peer.read(seed,callback,producer); Trace.hit(6);
        Value value=(Value)result; Trace.hit(7); return value;
    }
    public static Object raw(Core peer,Object seed,Callback callback,Factory producer) {
        Trace.hit(5); Object result=peer.read(seed,callback,producer); Trace.hit(6);
        Trace.hit(7); return result;
    }
    public static void discard(Core peer,Object seed,Callback callback,Factory producer) {
        Trace.hit(5); peer.read(seed,callback,producer); Trace.hit(6); Trace.hit(7);
    }
}
