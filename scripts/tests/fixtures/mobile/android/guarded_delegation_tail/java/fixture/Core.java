package fixture;

public class Core {
    public final int seed;
    public final long stamp;

    private Core(Context context, int value, long number) throws Throwable {
        Trace.leaked = this;
        seed = value;
        stamp = number;
        context.primary(this, value, number);
    }

    public Core(Context context, int salt, long number, Object... values) throws Throwable {
        this(context, context.choose(values) ? salt : salt + context.offset(values), number);
        for (int index = 0; index < values.length; ++index) {
            accept(context, index, values[index]);
        }
        context.finish(this, values);
    }

    public Core accept(Context context, int index, Object value) throws Throwable {
        context.step(this, index, value);
        return this;
    }
}
