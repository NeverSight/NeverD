import fixture.*;
public final class StarredHeadNominalCaller {
    public static Result call(Property property, Converter converter, Context context, boolean useConverter, int shape) {
        if (shape == 0) return Core.call(property, converter, context, useConverter);
        Core.discard(property, converter, context, useConverter); return null;
    }
}
