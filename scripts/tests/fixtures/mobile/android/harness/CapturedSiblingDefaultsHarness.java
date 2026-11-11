import fixture.Cursor;
import fixture.Extended;
import fixture.Owner;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.function.Consumer;

public final class CapturedSiblingDefaultsHarness {
    private static int values, identities, states, failures;
    private static void check(boolean condition) {
        ++values;
        if (!condition) throw new AssertionError("default value " + values);
    }
    private static void same(Object actual, Object expected) {
        ++identities;
        if (actual != expected) throw new AssertionError("default identity " + identities);
    }
    private static void state(Owner<?> owner, int calls, int marker) {
        ++states;
        if (owner.calls != calls || owner.marker != marker)
            throw new AssertionError("default state " + states);
    }
    private static void visit(int route, Owner<Object>.Child child,
                              Consumer<Object> consumer) throws Throwable {
        switch (route) {
        case 0: child.visit(consumer); return;
        case 1: { Owner<Object>.Base parent = child; parent.visit(consumer); return; }
        case 2: { Cursor<Object> cursor = child; cursor.visit(consumer); return; }
        case 3: { Extended<Object> cursor = child; cursor.visit(consumer); return; }
        case 4:
        case 5:
            try {
                (route == 4 ? Owner.Child.class : Cursor.class).getMethod("visit", Consumer.class)
                        .invoke(child, consumer);
                return;
            } catch (InvocationTargetException error) { throw error.getCause(); }
        default:
            MethodHandles.publicLookup().findVirtual(Owner.Child.class, "visit",
                    MethodType.methodType(void.class, Consumer.class)).invoke(child, consumer);
        }
    }
    private static final class Callback implements Consumer<Object> {
        final Owner<Object> owner;
        final Object token;
        final int expectedCalls, expectedMarker;
        final RuntimeException failure;
        int calls;
        Callback(Owner<Object> owner, Object token, int expectedCalls,
                 int expectedMarker, RuntimeException failure) {
            this.owner = owner;
            this.token = token;
            this.expectedCalls = expectedCalls;
            this.expectedMarker = expectedMarker;
            this.failure = failure;
        }
        public void accept(Object value) {
            ++calls;
            same(value, token);
            same(owner.value, token);
            state(owner, expectedCalls, expectedMarker);
            owner.marker = owner.marker * 7 + 1;
            owner.value = null;
            if (failure != null) throw failure;
        }
    }
    public static void main(String[] arguments) throws Throwable {
        CapturedSiblingAncestorsHarness.main(arguments);
        for (int route = 0; route < 7; ++route) {
            Object token = new Object();
            Owner<Object> owner = new Owner<>(null);
            Owner<Object>.Child child = owner.child(2);
            Callback idle = new Callback(owner, token, -1, -1, null);
            visit(route, child, idle);
            int calls = 1, marker = 2;
            state(owner, calls, marker);
            check(idle.calls == 0);
            owner.value = token;
            int expectedMarker = (marker * 3 + 2) * 3 + 2;
            Callback accepted = new Callback(owner, token, calls + 2, expectedMarker, null);
            visit(route, child, accepted);
            calls += 2;
            marker = expectedMarker * 7 + 1;
            state(owner, calls, marker);
            check(accepted.calls == 1);
            same(owner.value, null);
            owner.value = token;
            try { visit(route, child, null); throw new AssertionError("missing null callback"); }
            catch (NullPointerException expected) { ++failures; }
            calls += 2;
            marker = (marker * 3 + 2) * 3 + 2;
            state(owner, calls, marker);
            same(owner.value, token);
            RuntimeException outerFailure = new IllegalStateException("outer failure");
            owner.failure = outerFailure;
            try { visit(route, child, idle); throw new AssertionError("missing inherited failure"); }
            catch (IllegalStateException expected) { same(expected, outerFailure); ++failures; }
            ++calls;
            marker = marker * 3 + 2;
            state(owner, calls, marker);
            check(idle.calls == 0);
            owner.failure = null;
            RuntimeException callbackFailure = new IllegalArgumentException("callback failure");
            expectedMarker = (marker * 3 + 2) * 3 + 2;
            Callback failed = new Callback(owner, token, calls + 2, expectedMarker, callbackFailure);
            try { visit(route, child, failed); throw new AssertionError("missing callback failure"); }
            catch (IllegalArgumentException expected) { same(expected, callbackFailure); ++failures; }
            calls += 2;
            marker = expectedMarker * 7 + 1;
            state(owner, calls, marker);
            check(failed.calls == 1);
            same(owner.value, null);
            try { visit(route, null, idle); throw new AssertionError("missing null receiver"); }
            catch (NullPointerException expected) { ++failures; }
            state(owner, calls, marker);
            check(idle.calls == 0);
            check(Owner.Child.class.getMethod("visit", Consumer.class).getDeclaringClass() == Cursor.class);
        }
        System.out.println("default-values=" + values + ",identities=" + identities
                + ",states=" + states + ",failures=" + failures);
        System.out.println("default=unique-original,callback=ordered,capture=shared");
    }
}
