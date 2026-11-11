import fixture.CheckedTransform;
import fixture.Choice;
import fixture.Flag;
import fixture.Transform;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;

public final class CheckedEnumHarness {
    private static int calls, last;
    private static final RuntimeException SENTINEL = new RuntimeException("owned enum failure");
    private enum Alternate implements Flag {
        VALUE;
        public String text(int value) { ++calls; last = value; return "alternate"; }
    }
    private enum Failing implements Flag {
        VALUE;
        public String text(int value) { ++calls; throw SENTINEL; }
    }
    private enum Plain { VALUE }
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("Checked enum behavior changed");
    }
    public static void main(String[] args) throws Exception {
        int probes = 0;
        for (int bias : new int[] {-3, 0, 7}) {
            CheckedTransform target = new CheckedTransform(bias);
            Transform raw = target;
            for (boolean bridge : new boolean[] {false, true}) {
                for (Choice value : Choice.values()) {
                    String result = bridge ? (String) raw.apply(value) : target.apply(value);
                    require(result == (bias > 0 ? "positive" : "nonpositive"));
                    ++probes;
                }
                String result = bridge ? (String) raw.apply(Alternate.VALUE) : target.apply(Alternate.VALUE);
                require(result == "alternate" && last == bias);
                ++probes;
                try {
                    if (bridge) raw.apply(Failing.VALUE); else target.apply(Failing.VALUE);
                    throw new AssertionError("missing callback failure");
                } catch (RuntimeException actual) { require(actual == SENTINEL); }
                try {
                    if (bridge) raw.apply(Plain.VALUE); else target.apply(Plain.VALUE);
                    throw new AssertionError("missing body cast failure");
                } catch (ClassCastException actual) { require(actual.getMessage().contains("fixture.Flag")); }
                try {
                    if (bridge) raw.apply(null); else target.apply(null);
                    throw new AssertionError("missing null argument failure");
                } catch (NullPointerException expected) {}
            }
            try {
                raw.apply(new Object());
                throw new AssertionError("missing bridge cast failure");
            } catch (ClassCastException actual) { require(actual.getMessage().contains("java.lang.Enum")); }
        }
        require(calls == 12);
        CheckedTransform absent = null;
        try { absent.apply(Plain.VALUE); throw new AssertionError("missing null receiver failure"); }
        catch (NullPointerException expected) {}
        Method method = CheckedTransform.class.getDeclaredMethod("apply", Enum.class);
        ParameterizedType contract = (ParameterizedType) CheckedTransform.class.getGenericInterfaces()[0];
        Class<?> input = args[0].equals("original") ? Choice.class : Enum.class;
        require(method.getGenericParameterTypes()[0] == input && method.getReturnType() == String.class);
        require(contract.getActualTypeArguments()[0] == input);
        Method bridge = CheckedTransform.class.getDeclaredMethod("apply", Object.class);
        require(bridge.isBridge() && bridge.isSynthetic());
        System.out.println("identity-probes=" + probes + ";callback-calls=" + calls
                + ";callback-failures=6;body-cast-failures=6;bridge-cast-failures=3;null-argument-failures=6;null-receiver-failures=1");
    }
}
