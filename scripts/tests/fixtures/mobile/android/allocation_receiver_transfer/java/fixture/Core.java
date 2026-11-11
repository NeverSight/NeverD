package fixture;
public final class Core {
    public Core() {}
    public static Target make(Object first, Object second, Object third, Object fourth, boolean option,
        int n0, int n1, int n2, int n3, int n4, int n5, int n6, int n7, int n8, int n9, int n10, Owner owner) {
        return new Target(first, second, third, fourth, option, owner.flag);
    }
}
