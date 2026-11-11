package fixture;
public final class Provider {
    public static <T,V extends Bound> Object consume(Source<T,V> source,State<T,V> state,long tick,
            Action<? super Scope<T,V>,Unit> action,Completion<? super Unit> completion) {
        Trace.source=source;Trace.state=state;Trace.hit(1);
        state.value(source.value());
        state.vector(source.vector());
        state.tick(source.tick());
        state.running(source.running());
        Trace.hit(11);
        Scope<T,V> scope=new Scope<T,V>(source,state,tick);Trace.scope=scope;
        Unit unit=action.apply(scope);Trace.callback=unit;Trace.hit(12);
        completion.resume(unit);Trace.hit(13);
        return Trace.result;
    }
}
