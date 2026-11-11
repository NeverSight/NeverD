package fixture;
public final class Core {
    private Core() {}
    public static Subject<Object> array(Object[] input) {return new Subject<Object>(input);}
    public static Subject<String> strings(String[] input) {return new Subject<String>(input);}
    public static Subject<Object> direct(Object input) {return new Subject<Object>(input);}
    public static Object ignored(Object[] input) {new Subject<Object>(input); return Trace.marker;}
    public static Subject<Object> nullValue() {return new Subject<Object>((Object[])null);}
}
