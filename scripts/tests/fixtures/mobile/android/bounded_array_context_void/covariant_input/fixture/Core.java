package fixture;
public final class Core {
    private Core() {}
    public static void run(Object owner, Object name, Object input) {
        Context context = (Context)owner;
        String key = (String)name;
        java.util.List values = (java.util.List)input;
        Trace.hit(0, context, key, values, null, 0);
        Provider.write(context, key, values);
        Trace.hit(4, context, key, values, null, 0);
    }
    public static void typed(Object owner, Object name, Object input) {
        Context context = (Context)owner;
        String key = (String)name;
        java.util.List values = (java.util.List)input;
        Trace.hit(0, context, key, values, null, 0);
        Provider.<Mark>write(context, key, values);
        Trace.hit(4, context, key, values, null, 0);
    }
}
