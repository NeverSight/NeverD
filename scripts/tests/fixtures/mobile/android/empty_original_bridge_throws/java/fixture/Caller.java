package fixture;
public final class Caller {
    public static Box typed(Core receiver, Box value, int[] marker) throws First, Second {
        return receiver.visit(value, marker);
    }
    public static Object parent(Parent<Box> receiver, Box value, int[] marker) throws First, Second {
        return receiver.visit(value, marker);
    }
    public static Object api(Api<Box> receiver, Box value, int[] marker) throws First, Second {
        return receiver.visit(value, marker);
    }
}
