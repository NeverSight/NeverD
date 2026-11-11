package fixture;
public class Provider {
    static { Trace.hit(3); }
    public static <T> T select(Context context,
            Callback<? super Scope, ? super Completion<? super T>, ? extends Object> block)
            throws InterruptedException {
        Trace.context = context;
        Trace.callback = block;
        Trace.hit(4);
        Completion<T> completion = new Completion<T>();
        Trace.completion = completion;
        Scope scope = context.scope;
        Trace.scope = scope;
        Trace.hit(5);
        Object returned = block.invoke(scope, completion);
        Trace.returned = returned;
        Trace.hit(8);
        if (context.interruption != null) throw context.interruption;
        if (returned == Context.SUSPENDED) {
            if (!completion.done) throw new IllegalStateException("owned missing completion");
            if (completion.failure != null) throw completion.failure;
            return completion.value;
        }
        return (T) returned;
    }
}
