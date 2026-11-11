package fixture;
public final class Core {
    private Core() {}
    public static Object call(Subject receiver, Callback callback, Object value, int shape) {
        switch (shape) {
        case 0: return receiver.runZ(callback, value);
        case 1: return receiver.runI(callback, value);
        case 2: return receiver.runJ(callback, value);
        case 3: return receiver.runF(callback, value);
        case 4: return receiver.runD(callback, value);
        default: return null;
        }
    }
}
