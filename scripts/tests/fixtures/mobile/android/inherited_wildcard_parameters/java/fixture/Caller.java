package fixture;
public final class Caller {
    private Caller() { }
    public static boolean call(Core<Object,Object> core, Pair<Object,Object> entry) { return core.specificRemove(entry); }
}
