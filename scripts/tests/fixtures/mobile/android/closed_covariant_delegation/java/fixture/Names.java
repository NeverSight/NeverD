package fixture;
public final class Names {
    private Names() { }
    public static int events;
    public static Class<?> savedClass;
    public static String replacement, savedResult;
    public static Object prefixMarker;
    public static Probe prefixProbe, bodyProbe;
    public static RuntimeException prefixFailure, bodyFailure;
    public static String key(Class<?> type) {
        events = events * 5 + 1;
        savedClass = type;
        prefixMarker = prefixProbe.observe(type, replacement);
        if (prefixFailure != null) throw prefixFailure;
        savedResult = replacement;
        events = events * 5 + 2;
        return replacement;
    }
}
