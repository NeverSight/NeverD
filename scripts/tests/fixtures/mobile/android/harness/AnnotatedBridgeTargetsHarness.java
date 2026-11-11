import fixture.*;
import java.lang.reflect.*;

public class AnnotatedBridgeTargetsHarness {
    static int values, identities, failures;
    static void check(boolean value) { values++; if (!value) throw new AssertionError("value " + values); }
    static void same(Object a, Object b) { identities++; if (a != b) throw new AssertionError("identity " + identities); }
    static Object get(int entry, Core owner, Object tag) {
        return entry == 0 ? owner.get(tag) : ((Api<String>) owner).get(tag);
    }
    public static void main(String[] args) throws Exception {
        Core[] owners = {new Core(), new Core()};
        String[] selected = {new String("value"), new String("value"), null};
        Object[] tags = {new Object(), "tag", null};
        for (Core owner : owners) for (String result : selected) for (Object tag : tags) {
            owner.selected = result; owner.calls = 0;
            for (int entry = 0; entry < 2; entry++) {
                same(get(entry, owner, tag), result);
                same(owner.selected, result); same(owner.tag, tag);
                check(owner.calls == entry + 1);
            }
        }
        for (int entry = 0; entry < 2; entry++) {
            Core owner = owners[0];
            RuntimeException sentinel = new RuntimeException("owned annotated target failure");
            Object tag = new Object();
            owner.calls = 0; owner.failure = sentinel;
            try { get(entry, owner, tag); throw new AssertionError("missing sentinel"); }
            catch (RuntimeException failure) {
                failures++; same(failure, sentinel); same(owner.tag, tag); check(owner.calls == 1);
            }
            owner.calls = 0; owner.tag = null;
            try { get(entry, null, tag); throw new AssertionError("missing null receiver"); }
            catch (RuntimeException failure) {
                failures++; check(failure instanceof NullPointerException); check(owner.calls == 0); same(owner.tag, null);
            }
            owner.failure = null;
        }
        int targets = 0, bridges = 0;
        for (Method method : Core.class.getDeclaredMethods()) {
            if (!method.getName().equals("get")) continue;
            Stamp stamp = method.getAnnotation(Stamp.class);
            if (method.isBridge()) {
                bridges++; check(method.isSynthetic());
                same(method.getReturnType(), Object.class);
                check((stamp != null) == (args.length == 1));
                if (stamp != null && (stamp.value() != 11 || !stamp.label().equals("owned")))
                    throw new AssertionError("copied bridge annotation values changed");
            } else {
                targets++; check(stamp != null);
                check(stamp.value() == 11); check(stamp.label().equals("owned"));
                same(method.getReturnType(), String.class);
            }
        }
        check(targets == 1); check(bridges == 1);
        ParameterizedType contract = (ParameterizedType) Core.class.getGenericInterfaces()[0];
        same(contract.getRawType(), Api.class); same(contract.getActualTypeArguments()[0], String.class);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("target-annotations-and-ordered-effects=retained,bridge-annotation-copy=reported");
    }
}
