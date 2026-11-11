package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Caller {
    public static int narrow(Carrier self, long other) { return self.compareScalar(other); }
    public static int wide(Comparable self, Object other) { return self.compareTo(other); }
}
