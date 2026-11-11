package fixture;
public final class Caller {
    private Caller() { }
    public static void put(Core<Object> core, Object item) { core.append(item); }
    public static boolean scalar(Base<Object> core, Object item) { return core.add(item); }
}
