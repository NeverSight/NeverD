package fixture;
public final class NestedPrefixAllocationCaller {
    public static Core create(int operation,Object head,Object first,Object second) {
        if (operation==0) return new Core(head,first,second);
        return new Core(head,first,second,7);
    }
}
