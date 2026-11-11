import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public final class FinalParameterCovariantResultHarness {
    private static int identities, effects, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("reference changed");
        identities++;
    }
    private static void events(int expected) {
        if (Trace.events != expected) throw new AssertionError("effect order changed");
        Trace.events = 0;
        effects++;
    }
    private static void failure(Runnable call, Class<? extends Throwable> kind,
                                RuntimeException same, int count) {
        try { call.run(); throw new AssertionError("failure missing"); }
        catch (Throwable error) {
            if (error.getClass() != kind || (same != null && error != same))
                throw new AssertionError("failure type or identity changed", error);
            if (same != null) identities++;
            failures++;
        }
        events(count);
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void main(String[] args) throws Exception {
        Receiver<Object> receiver = new Receiver<>();
        Function raw = receiver;
        List[] lists = {null, new ArrayList(), Arrays.asList(new Value()),
                        Arrays.asList(new Value(), new Value()), Arrays.asList("erased leaf")};
        for (int iteration = 0; iteration < 8; iteration++)
            for (List first : lists)
                for (List second : lists) {
                    identity(raw.invoke(first, second), first == null ? second : first);
                    identity(receiver.first, first); identity(receiver.second, second); events(1);
                    identity(receiver.invoke(first, second), first == null ? second : first);
                    identity(receiver.first, first); identity(receiver.second, second); events(1);
                }
        failure(() -> raw.invoke("bad", lists[1]), ClassCastException.class, null, 0);
        failure(() -> raw.invoke(lists[1], "bad"), ClassCastException.class, null, 0);
        RuntimeException problem = new IllegalStateException("owned covariant list failure");
        Trace.targetFailure = problem;
        failure(() -> raw.invoke(lists[1], lists[2]), IllegalStateException.class, problem, 1);
        failure(() -> receiver.invoke(lists[1], lists[2]), IllegalStateException.class, problem, 1);
        Trace.targetFailure = null;
        Receiver<?> absent = null;
        failure(() -> absent.invoke((Object) lists[1], (Object) lists[2]), NullPointerException.class, null, 0);
        failure(() -> absent.invoke(lists[1], lists[2]), NullPointerException.class, null, 0);
        Method target = Receiver.class.getMethod("invoke", List.class, List.class);
        boolean rebuilt = args[0].equals("rebuilt");
        for (Type parameter : target.getGenericParameterTypes()) {
            Type leaf = ((ParameterizedType) parameter).getActualTypeArguments()[0];
            if (rebuilt ? !(leaf instanceof WildcardType) ||
                !Arrays.equals(((WildcardType) leaf).getUpperBounds(), new Type[] {Value.class}) :
                leaf != Value.class) throw new AssertionError("parameter projection differs from report");
        }
        if (((ParameterizedType) target.getGenericReturnType()).getActualTypeArguments()[0] != Value.class)
            throw new AssertionError("original covariant return Signature changed");
        for (String field : new String[] {"first", "second"})
            if (((ParameterizedType) Receiver.class.getField(field).getGenericType()).getActualTypeArguments()[0] != Value.class)
                throw new AssertionError("original field Signature changed");
        Method bridge = Receiver.class.getMethod("invoke", Object.class, Object.class);
        if (!bridge.isBridge() || !bridge.isSynthetic() ||
            !(Receiver.class.getGenericInterfaces()[0] instanceof ParameterizedType))
            throw new AssertionError("complete interface or erased bridge flags changed");
        System.out.println("identities=" + identities + ",effects=" + effects + ",failures=" + failures);
        System.out.println(rebuilt ? "reflection=reported-final-parameters,original-return=retained" : "reflection=original");
    }
}
