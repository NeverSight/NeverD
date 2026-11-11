package fixture;
public class Target {
    public Context context;
    public Callback callback;
    public boolean result;
    static { Trace.hit(2); Trace.initializer.run(); }
    public Target(Context context, Callback callback) {
        Trace.hit(4);
        Trace.constructed = this;
        this.context = context;
        this.callback = callback;
        Trace.constructorAction.run();
        result = callback.call(Trace.input);
        Trace.hit(7);
    }
}
