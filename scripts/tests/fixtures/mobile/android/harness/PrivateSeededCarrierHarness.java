import fixture.Carrier;
import fixture.Core;

public final class PrivateSeededCarrierHarness {
    private static final class FutureCarrier extends Carrier<Object> {
        FutureCarrier(Object value) { super(value); }
    }

    private static void check(boolean value) {
        if (!value) throw new AssertionError("owned private seeded carrier");
    }

    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]);
        int mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; ++attempt) {
            Core core = new Core();
            Object sentinel = new Object();
            Object input = (mode & 8) == 0 ? new Object() : null;
            String key = (mode & 8) == 0 ? new String("owned key") : null;
            Object payload = (mode & 2) == 0 ? new String("owned value") : new Object();
            Carrier carrier = (mode & 1) == 0 ? new FutureCarrier(payload) : null;
            RuntimeException expected = new RuntimeException("owned factory failure");
            boolean fail = (mode & 4) != 0;
            boolean flag = attempt == 0;
            core.saved = sentinel;
            core.replacement = carrier;
            core.failure = fail ? expected : null;
            Carrier result = null;
            boolean returned = false;
            try {
                if (route == 0) result = core.call(key, flag, input);
                else if (route == 1) result = core.empty(key);
                else if (route == 2) core.discard(key, flag, input);
                else ((Core) null).call(key, flag, input);
                returned = true;
                check(route != 3 && !fail);
            } catch (RuntimeException error) {
                check(route == 3 ? error instanceof NullPointerException : fail && error == expected);
            }
            int events = route == 3 ? 0 : route == 0 ? (fail ? 16 : 414) : (fail ? 1 : 7);
            check(core.events == events);
            check(returned == (route != 3 && !fail));
            if (route == 3) {
                check(core.saved == sentinel && core.key == null && core.seed == null && !core.flag);
            } else {
                check(core.key == key);
                check(core.seed == (route == 1 ? null : input));
                check(core.flag == (route == 1 ? false : flag));
                check(core.saved == (fail ? sentinel : carrier));
                check(result == (!fail && route != 2 ? carrier : null));
                if (returned && route != 2) {
                    try {
                        String value = ((Carrier<String>) result).value;
                        check(carrier != null && payload instanceof String && value == payload);
                    } catch (NullPointerException error) {
                        check(carrier == null);
                    } catch (ClassCastException error) {
                        check(carrier != null && !(payload instanceof String));
                    }
                }
            }
            check(core.events == events);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
