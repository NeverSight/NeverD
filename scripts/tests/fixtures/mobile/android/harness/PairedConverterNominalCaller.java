import fixture.*;
public final class PairedConverterNominalCaller {
    public static Result call(Object value, Property property, Converter converter, Context context, int shape) {
        if (shape == 0) return Core.call(value, property, converter, context);
        Core.discard(value, property, converter, context); return null;
    }
}
