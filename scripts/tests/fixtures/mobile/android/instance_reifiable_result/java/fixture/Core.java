package fixture;

public final class Core {
    private Core() {}
    public static Object direct(Provider provider, String name) { return provider.<Object>read(name); }
    public static String narrow(Provider provider, String name) { return provider.<String>read(name); }
    public static Object quiet(Provider provider, String name) { return provider.<Object>quiet(name); }
    public static String quietNarrow(Provider provider, String name) { return provider.<String>quiet(name); }
    public static Object[] array(Provider provider, String name) { return provider.<Object[]>read(name); }
    public static Object ignored(Provider provider, String name) { provider.<Object>read(name); return Trace.marker; }
    public static Object noKey(Provider provider) { return provider.<Object>empty(); }
    public static Class<?> classValue(Provider provider, String name) { return provider.<Class<?>>read(name); }
}
