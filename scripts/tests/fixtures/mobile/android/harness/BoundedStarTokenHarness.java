import fixture.*;
import java.lang.reflect.*;

public final class BoundedStarTokenHarness {
    static int mode;
    static final class FutureFactory extends Factory {}
    static void check(boolean value) {
        if (!value) throw new AssertionError("owned bounded star token invariant");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    static void poison(Leaf value, Bound poison) { ((Carrier) value).value = poison; }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        final int route = Integer.parseInt(args[0]);
        mode = Integer.parseInt(args[1]);
        Method get = Factory.class.getMethod("get", Class.class);
        TypeVariable<?> formal = get.getTypeParameters()[0];
        ParameterizedType bound = (ParameterizedType) formal.getBounds()[0];
        check(bound.getRawType() == Carrier.class && bound.getActualTypeArguments()[0] instanceof WildcardType);
        check(get.getGenericReturnType().equals(formal)
                && ((ParameterizedType) get.getGenericParameterTypes()[0]).getActualTypeArguments()[0].equals(formal));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Factory.events = 0;
            final Factory factory = new FutureFactory();
            final RuntimeException callbackFailure = new RuntimeException();
            final RuntimeException targetFailure = new RuntimeException();
            final Error callbackError = new AssertionError("callback sentinel");
            final Payload payload = mode == 10 ? null : new Payload();
            final Leaf initial = new Leaf(), alternate = new Leaf();
            initial.value = payload;
            alternate.value = new Payload();
            final Carrier<Bound> sibling = new Carrier<Bound>();
            sibling.value = new Bound();
            final Carrier<?> sentinel = new Carrier<Bound>();
            final Object marker = mode == 15 ? null : new Object();
            final Class token = mode == 12 ? null : mode == 14 ? Carrier.class : Leaf.class;
            final int[] calls = {0};
            if (mode == 11) poison(initial, new Bound());
            factory.replacement = mode == 6 ? null : mode == 7 ? sibling : initial;
            factory.savedResult = sentinel;
            factory.failure = mode == 5 || mode == 13 ? targetFailure : null;
            factory.probe = mode == 2 ? null : new Probe() {
                public Object observe(Factory owner, Class actualToken) {
                    check(++calls[0] == 1 && owner == factory && actualToken == token);
                    check(Factory.events == 16 && factory.savedToken == token
                            && factory.marker == null && factory.savedResult == sentinel);
                    if (mode == 3 || mode == 13) throw callbackFailure;
                    if (mode == 4) throw callbackError;
                    if (mode == 8) factory.replacement = alternate;
                    if (mode == 9) factory.failure = targetFailure;
                    return marker;
                }
            };
            Carrier<?> result = null;
            Throwable failure = null;
            try {
                Factory supplied = mode == 1 ? null : factory;
                if (route == 0) result = Core.call(supplied, token);
                else if (route == 1) Core.discard(supplied, token);
                else if (route == 2) result = Core.typed(supplied, token);
                else result = Core.narrowed(supplied, token);
            } catch (Throwable caught) { failure = caught; }
            boolean providerSuccess = mode != 1 && mode != 2 && mode != 3 && mode != 4
                    && mode != 5 && mode != 9 && mode != 13;
            boolean narrowedFailure = providerSuccess && mode == 7 && route == 3;
            int events = mode == 1 ? 3 : !providerSuccess ? 16 : route == 3 ? 2076 : 414;
            check(Factory.events == events && calls[0] == (mode == 1 || mode == 2 ? 0 : 1));
            if (mode == 1 || mode == 2) check(failure instanceof NullPointerException);
            else if (mode == 3 || mode == 13) check(failure == callbackFailure);
            else if (mode == 4) check(failure == callbackError);
            else if (mode == 5 || mode == 9) check(failure == targetFailure);
            else if (narrowedFailure) check(failure instanceof ClassCastException);
            else check(failure == null);
            Carrier<?> expected = mode == 8 ? alternate : mode == 6 ? null : mode == 7 ? sibling : initial;
            check(factory.savedToken == (mode == 1 ? null : token));
            check(factory.marker == (mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 13 ? null : marker));
            check(factory.savedResult == (providerSuccess ? expected : sentinel));
            check(result == (providerSuccess && !narrowedFailure && route != 1 ? expected : null));
            if (providerSuccess && expected instanceof Leaf) {
                if (mode == 11) {
                    boolean bad = false;
                    try { Payload ignored = ((Leaf) expected).value; }
                    catch (ClassCastException caught) { bad = true; }
                    check(bad && Factory.events == events);
                } else check(((Leaf) expected).value == (mode == 8 ? alternate.value : payload));
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
