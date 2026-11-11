package fixture;
public final class Core {
    private Core() {}
    public static Object call(Subject receiver, Object value, int shape) {
        switch (shape) {
        case 0: return receiver.runZ(value);
        case 1: return receiver.runB(value);
        case 2: return receiver.runC(value);
        case 3: return receiver.runS(value);
        case 4: return receiver.runI(value);
        case 5: return receiver.runJ(value);
        case 6: return receiver.runF(value);
        case 7: return receiver.runD(value);
        default: throw new IllegalArgumentException();
        }
    }
}
