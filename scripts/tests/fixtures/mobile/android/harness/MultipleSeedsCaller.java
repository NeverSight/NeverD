import fixture.Client;
import fixture.State;
public final class MultipleSeedsCaller {
    public static void call(int route, State state, int first, Object a, int second, Object b,
                            int third, Object c, int fourth, Object d) {
        switch (route) {
        case 0: Client.direct3(state, first, a, second, b, third, c); break;
        case 1: Client.inherited3(state, first, a, second, b, third, c); break;
        case 2: Client.direct4(state, first, a, second, b, third, c, fourth, d); break;
        case 3: Client.inherited4(state, first, a, second, b, third, c, fourth, d); break;
        default: throw new AssertionError("invalid owned route");
        }
    }
}
