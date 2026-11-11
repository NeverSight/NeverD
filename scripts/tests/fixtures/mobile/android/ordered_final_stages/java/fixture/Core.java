package fixture;
public final class Core {
    public final int prefix;
    public final boolean choice;
    public final Object payload;
    public final long tail;
    public final Object marker;

    public Core(Context context, long seed, int selector, Object token) throws Throwable {
        Trace.leaked = this;
        prefix = context.enter(this, seed, token);
        try {
            if (context.choose(this, selector)) {
                choice = true;
                payload = context.left(this, seed, token);
            } else {
                choice = false;
                payload = context.right(this, seed, token);
            }
        } finally {
            context.cleanup(this, token);
        }
        tail = context.finish(this, seed);
        marker = context.marker(this, token);
    }
}
