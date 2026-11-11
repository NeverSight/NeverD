import fixture.Outer;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;

public class GuardedConstructorCaptureHarness {
    public static void main(String[] args) throws Exception {
        Constructor<Outer.Member> ctor = Outer.Member.class.getConstructor(
                Outer.class, int.class, int.class, int.class, int.class, Object.class);
        Field capture = Outer.Member.class.getDeclaredField("this$0");
        capture.setAccessible(true);
        int values = 0, identities = 0, failures = 0;
        for (int value : new int[] {Integer.MIN_VALUE, -13, 0, 17, Integer.MAX_VALUE}) {
            for (int flags = 0; flags < 16; flags++) {
                for (int mode = 0; mode < 3; mode++) {
                    Outer host = mode == 2 ? null : new Outer();
                    if (host != null) { host.limit = value ^ 137; host.fail = mode == 1; }
                    Outer.getters = 0;
                    Outer.constructions = 0;
                    boolean fails = (flags & 4) != 0 && mode != 0;
                    try {
                        Outer.Member member = ctor.newInstance(host, value, ~value, value + 1, flags, new Object());
                        if (fails) throw new AssertionError("getter failure was bypassed");
                        int first = (flags & 1) == 0 ? value : 0;
                        int second = (flags & 2) == 0 ? ~value : 0;
                        int third = (flags & 4) == 0 ? value + 1 : host.limit;
                        if (member.first != first || member.second != second || member.third != third)
                            throw new AssertionError("guarded defaults changed final values");
                        if (capture.get(member) != host || member.owner() != host || Outer.constructions != 1)
                            throw new AssertionError("capture identity or construction count changed");
                        values += 3; identities++;
                    } catch (InvocationTargetException error) {
                        if (!fails || Outer.constructions != 0)
                            throw new AssertionError("failure occurred after initialization", error);
                        if (host == null ? !(error.getCause() instanceof NullPointerException)
                                         : error.getCause() != host.sentinel)
                            throw new AssertionError("getter failure identity changed", error);
                        failures++;
                    }
                    int getterCount = (flags & 4) == 0 || host == null ? 0 : 1;
                    if (Outer.getters != getterCount)
                        throw new AssertionError("getter evaluation count changed");
                }
            }
        }
        if (values != 480 || identities != 160 || failures != 80)
            throw new AssertionError("guarded constructor coverage changed");
        System.out.println("values=" + values + ",outer-identities=" + identities + ",failures=" + failures);
        System.out.println("getter-order-and-count=matched,construction-count=matched");
        System.out.println("static=" + Modifier.isStatic(Outer.Member.class.getModifiers()));
    }
}
