package fixture;

public final class Core {
    public final int prefix;
    public final boolean choice;
    public final Object payload;
    public final long tail;

    public Core(Context context, long seed, int rounds, boolean branch, Object token) throws Throwable {
        Trace.leaked = this;
        prefix = context.enter(this, seed, token);
        for (int i = 0; i < rounds; ++i) {
            context.step(this, i, token);
        }
        if (rounds == Integer.MIN_VALUE) {
            context.spin(this, token);
            for (;;) {}
        }
        if (context.choose(this, branch)) {
            choice = true;
            payload = context.left(this, seed, token);
        } else {
            choice = false;
            payload = context.right(this, seed, token);
        }
        tail = context.finish(this, seed);
    }
}
