import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.WildcardType;

public final class ScopedRawParametersHarness {
    private static int values, identities, states, failures;
    private static Method method;
    private static MethodHandle handle;
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed");
        identities++;
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("state changed");
        states++;
    }
    private static void call(int route, Api<RawBox<? extends String>> receiver,
                             RawBox<Object> box, Object tag) throws Throwable {
        try {
            switch (route) {
                case 0: receiver.accept(box, tag); return;
                case 1: handle.invokeExact((Api) receiver, (RawBox) box, tag); return;
                case 2: method.invoke(receiver, box, tag); return;
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void metadata() throws Exception {
        method = Api.class.getMethod("accept", RawBox.class, Object.class);
        handle = MethodHandles.lookup().findVirtual(Api.class, "accept",
                MethodType.methodType(void.class, RawBox.class, Object.class));
        WildcardType argument = (WildcardType) ((ParameterizedType) method.getGenericParameterTypes()[0]).getActualTypeArguments()[0];
        if (argument.getLowerBounds()[0] != Api.class.getTypeParameters()[0])
            throw new AssertionError("interface parameter lost its original formal");
        for (Class<?> owner : new Class<?>[]{Forwarder.class, Leaf.class}) {
            ParameterizedType api = (ParameterizedType) owner.getGenericInterfaces()[0];
            ParameterizedType box = (ParameterizedType) api.getActualTypeArguments()[0];
            WildcardType nested = (WildcardType) box.getActualTypeArguments()[0];
            TypeVariable<?> variable = (TypeVariable<?>) nested.getUpperBounds()[0];
            if (api.getRawType() != Api.class || box.getRawType() != RawBox.class
                    || variable != owner.getTypeParameters()[0] || variable.getGenericDeclaration() != owner
                    || nested.getLowerBounds().length != 0 || owner.getTypeParameters().length != 2)
                throw new AssertionError("complete nested nominal scope changed");
        }
    }
    public static void main(String[] args) throws Throwable {
        OriginalRawParametersHarness.main(args);
        metadata();
        Sink sink = new Sink();
        Forwarder<String,Integer> direct = new Forwarder<String,Integer>(sink);
        Leaf<String,Integer> inherited = new Leaf<String,Integer>();
        for (int provider = 0; provider < 2; provider++) {
            Api<RawBox<? extends String>> receiver = provider == 0 ? direct : inherited;
            for (int route = 0; route < 3; route++) {
                for (int n = 0; n < 4; n++) {
                    RawBox<Object> box = new RawBox<Object>(n == 0 ? null : new Object());
                    Object tag = n == 1 ? null : new Object();
                    int before = provider == 0 ? sink.calls : inherited.calls;
                    call(route, receiver, box, tag);
                    state(before + 1, provider == 0 ? sink.calls : inherited.calls);
                    state(provider == 0 ? 31 : 2, Trace.events); Trace.events = 0;
                    RawBox<?> seen = provider == 0 ? sink.seen : inherited.seen;
                    same(box, seen); same(tag, provider == 0 ? sink.tag : inherited.tag);
                    same(box.value, seen.value);
                    box.value = new Object(); same(box.value, seen.value);
                    values++;
                }
                RuntimeException problem = new RuntimeException("owned-scoped-parameter");
                if (provider == 0) sink.failure = problem; else inherited.failure = problem;
                RawBox<Object> box = new RawBox<Object>(new Object()); Object tag = new Object();
                int before = provider == 0 ? sink.calls : inherited.calls;
                try { call(route, receiver, box, tag); throw new AssertionError("failure missing"); }
                catch (RuntimeException error) { same(problem, error); failures++; }
                state(before + 1, provider == 0 ? sink.calls : inherited.calls);
                state(provider == 0 ? 31 : 2, Trace.events); Trace.events = 0;
                same(box, provider == 0 ? sink.seen : inherited.seen);
                same(tag, provider == 0 ? sink.tag : inherited.tag);
                if (provider == 0) sink.failure = null; else inherited.failure = null;
                try { call(route, null, box, tag); throw new AssertionError("null receiver accepted"); }
                catch (NullPointerException error) { failures++; }
                state(0, Trace.events);
            }
        }
        System.out.println("scoped-values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures + ",bridges=0");
        System.out.println("complete-scoped-nominal-parameters-and-original-raw-methods=retained");
    }
}
