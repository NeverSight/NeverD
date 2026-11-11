package fixture;
public final class Core {
    private Core() {}
    public static <T> String call(T[] values,CharSequence delimiter){return Provider.join(values,delimiter);}
    public static String checked(Object values,CharSequence delimiter){return call((Object[])values,delimiter);}
}
