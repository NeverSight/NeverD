import fixture.Client;
import fixture.State;
public final class DirectSeedsCaller {
    public static void call(int route, State state, int first, Object a, int second, Object b, long bits) {
        switch (route) {
        case 0: Client.direct(state, first, a, second, b); break;
        case 1: Client.inherited(state, first, a, second, b); break;
        case 2: Client.wide(state, first, a, second, b, bits); break;
        case 3: Client.inheritedWide(state, first, a, second, b, bits); break;
        default: throw new AssertionError("invalid owned route");
        }
    }
}
