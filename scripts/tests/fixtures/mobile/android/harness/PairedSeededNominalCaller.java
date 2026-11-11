import fixture.*;
public final class PairedSeededNominalCaller {
    public static Object call(Object value, Property property, Context context, int shape) {
        if (shape == 0) return Core.call(value, property, context);
        Core.discard(value, property, context); return null;
    }
}
