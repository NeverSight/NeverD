package fixture;
public final class Caller {
    public static Api typed(Api self, long delta) { return self.shift(delta); }
    public static Parent parent(Parent self, long delta) { return self.shift(delta); }
    public static long scalar(Core self, long delta) { return self.scalarShift(delta); }
}
