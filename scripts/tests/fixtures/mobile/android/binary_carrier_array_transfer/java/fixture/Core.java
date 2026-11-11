package fixture;

public final class Core {
    private Core() {}
    public static Box raw(Box input, Object[] values) { return Provider.first(input, values); }
    public static Box<String,Integer> typed(Box<String,Integer> input, String[] values) { return Provider.first(input, values); }
    public static Object ignored(Box input, Object[] values) {
        Provider.first(input, values);
        return Trace.marker;
    }
    public static Box second(Box input, Object[] values) { return Provider.second(input, values); }
    public static Box fixed(Box input, Object[] values) { return Provider.fixed(input, values); }
}
