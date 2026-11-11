import fixture.*;
import java.lang.reflect.*;

public class OriginalRawResultsHarness {
    static int values, identities, failures;
    static void check(boolean value) { values++; if (!value) throw new AssertionError("value " + values); }
    static void same(Object a, Object b) { identities++; if (a != b) throw new AssertionError("identity " + identities); }
    static RawBox<?> get(int entry, RawOwner owner, RawTag input) {
        return entry == 0 ? owner.get(input) : ((RawApi) owner).get(input);
    }
    public static void main(String[] args) throws Exception {
        RawOwner owner = new RawOwner();
        for (int entry = 0; entry < 2; entry++) for (RawTag input : new RawTag[] {new RawTag(-7), new RawTag(9), null}) {
            RawFactory.calls = 0; RawFactory.last = null;
            RawBox<?> first = get(entry, owner, input);
            RawBox<?> second = get(entry, owner, input);
            check(first != null); check(second != null); check(first != second);
            same(first.value, input); same(second.value, input); same(RawFactory.last, input);
            check(RawFactory.calls == 2);
            if (input != null) check(((RawTag) first.value).value == input.value);
        }
        RawBox<RawTag> box = new RawBox<RawTag>();
        owner.accept(box); same(owner.accepted, box); check(owner.accepts == 1);
        ((RawApi) owner).accept(null); same(owner.accepted, null); check(owner.accepts == 2);
        for (int entry = 0; entry < 2; entry++) {
            RawTag tag = new RawTag(31);
            RuntimeException sentinel = new RuntimeException("owned raw-result failure");
            RawFactory.calls = 0; RawFactory.last = null; RawFactory.failure = sentinel;
            try { get(entry, owner, tag); throw new AssertionError("missing sentinel"); }
            catch (RuntimeException failure) { failures++; same(failure, sentinel); }
            same(RawFactory.last, tag); check(RawFactory.calls == 1);
            RawFactory.failure = null; RawFactory.calls = 0; RawFactory.last = null;
            try { get(entry, null, tag); throw new AssertionError("missing null receiver"); }
            catch (NullPointerException expected) { failures++; }
            check(RawFactory.calls == 0); same(RawFactory.last, null);
        }
        Method raw = RawOwner.class.getDeclaredMethod("get", RawTag.class);
        same(raw.getGenericReturnType(), RawBox.class);
        check(raw.getTypeParameters().length == 0); check(!raw.isBridge());
        ParameterizedType required = (ParameterizedType) RawApi.class.getDeclaredMethod("get", RawTag.class).getGenericReturnType();
        same(required.getRawType(), RawBox.class); same(required.getActualTypeArguments()[0], RawTag.class);
        for (Class<?> declaration : new Class<?>[] {RawApi.class, RawOwner.class}) {
            check(declaration.getTypeParameters().length == 0);
            Method accept = declaration.getDeclaredMethod("accept", RawBox.class);
            ParameterizedType parameter = (ParameterizedType) accept.getGenericParameterTypes()[0];
            same(parameter.getRawType(), RawBox.class); same(parameter.getActualTypeArguments()[0], RawTag.class);
            same(accept.getReturnType(), void.class);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("original-raw-results-and-complete-parameters=retained,allocation-dispatch-and-effect-order=matched");
    }
}
