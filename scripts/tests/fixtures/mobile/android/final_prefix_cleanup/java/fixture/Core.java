package fixture;
public final class Core extends Parent {
    public final Object first, second;
    public Core(Context context, boolean choice, Object a, Object b) throws Throwable {
        super();
        first = context.first(this, a);
        second = context.second(this, b);
        try {
            context.enter(this);
            if (context.choose(this, choice)) context.step(this);
            else throw Trace.rejection;
        } finally {
            context.cleanup(this);
        }
        context.after(this);
    }
}
