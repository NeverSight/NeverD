package fixture;
public final class Client {
    public static Object call(boolean flag) { Factory<?,?> value=Factory.create(flag);Trace.after=value;Trace.hit(3);return value; }
    public static String value(boolean flag) { Factory<?,String> value=Factory.create(flag);Trace.after=value;Trace.hit(3);return value.map.get(Trace.key); }
    public static String key(boolean flag) { Factory<String,?> value=Factory.create(59,flag);Trace.after=value;Trace.hit(3);return value.map.keySet().iterator().next(); }
}
