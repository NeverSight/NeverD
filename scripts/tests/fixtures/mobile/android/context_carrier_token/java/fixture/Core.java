package fixture;

public final class Core {
    private Core() {}
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static String run(Object owner, Object name, Object type) {
        Context context = (Context)owner;
        String key = (String)name;
        Class token = (Class)type;
        Trace.hit(3, context, key, token, null);
        Box result = Provider.read(context, key, token);
        Trace.hit(4, context, key, token, result);
        return (String)result.item;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void ignore(Object owner, Object name, Object type) {
        Context context = (Context)owner;
        String key = (String)name;
        Class token = (Class)type;
        Trace.hit(3, context, key, token, null);
        Provider.read(context, key, token);
        Trace.hit(4, context, key, token, null);
    }
}
