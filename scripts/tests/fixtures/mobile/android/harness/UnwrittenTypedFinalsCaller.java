import fixture.Owner;
import fixture.View;

public final class UnwrittenTypedFinalsCaller {
    public static View get() { return Owner.get(); }

    public static Object read(View value, int kind) {
        switch (kind) {
            case 0: return value.readZ();
            case 1: return value.readB();
            case 2: return value.readC();
            case 3: return value.readS();
            case 4: return value.readI();
            case 5: return value.readJ();
            case 6: return value.readF();
            case 7: return value.readD();
            case 8: return value.readO();
            case 9: return value.readA();
            default: throw new AssertionError();
        }
    }
}
