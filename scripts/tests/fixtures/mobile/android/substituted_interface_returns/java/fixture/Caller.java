package fixture;
public final class Caller {
    public static Item narrow(Narrow self) { return self.next(); }
    public static Object wide(java.util.Iterator<Item> self) { return self.next(); }
}
