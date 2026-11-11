package fixture;
public final class Core {
    public Core() {}
    @SuppressWarnings("unchecked")
    public static Result call(Object value, Property property, Converter converter, Context context) {
        return Provider.make(value, property, converter, context);
    }
    @SuppressWarnings("unchecked")
    public static void discard(Object value, Property property, Converter converter, Context context) {
        Provider.make(value, property, converter, context);
    }
}
