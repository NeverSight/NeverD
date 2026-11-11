package fixture;
public final class Core {
    public Core() {}
    @SuppressWarnings("unchecked")
    public static Result call(Property property, Converter converter, Context context, boolean useConverter) {
        if (useConverter) return Provider.make(property, converter, context);
        return Bridge.Factory.make(property, context);
    }
    @SuppressWarnings("unchecked")
    public static void discard(Property property, Converter converter, Context context, boolean useConverter) {
        if (useConverter) Provider.make(property, converter, context);
        else Bridge.Factory.make(property, context);
    }
}
