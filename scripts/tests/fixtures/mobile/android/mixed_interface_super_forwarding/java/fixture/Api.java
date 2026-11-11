package fixture;
public interface Api extends Shared {
    default int measure(Context context, Input input, int value) {
        Trace.begin(this, context, input, value);
        int result = input.measure(value ^ context.offset() ^ base());
        Trace.after(result);
        return result;
    }
    default long measure(Context context, Input input, long value) {
        Trace.begin(this, context, input, value);
        long result = input.measure(value ^ context.wideOffset() ^ base());
        Trace.after(result);
        return result;
    }
}
