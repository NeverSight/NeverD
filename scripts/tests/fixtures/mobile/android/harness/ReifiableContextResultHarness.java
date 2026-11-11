import fixture.*;
import java.lang.reflect.*;
public final class ReifiableContextResultHarness {
    static int mode;
    static final class FutureFactory extends Factory {
        int calls;
        Carrier<?> returned, alternate;
        RuntimeException afterFailure;
        @Override @SuppressWarnings("unchecked")
        public <T extends Carrier<?>> T get(String key) {
            calls++;
            T value = super.get(key);
            returned = value;
            Factory.events = Factory.events * 5 + 7;
            if (mode == 13) throw afterFailure;
            return mode == 14 ? (T) alternate : value;
        }
    }
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned reifiable context result invariant");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    static void poison(Leaf leaf, Bound value) { ((Carrier) leaf).value = value; }
    public static void main(String[] args) throws Exception {
        final int route = Integer.parseInt(args[0]);
        mode = Integer.parseInt(args[1]);
        Method get = Factory.class.getMethod("get", String.class);
        TypeVariable<?> formal = get.getTypeParameters()[0];
        ParameterizedType bound = (ParameterizedType) formal.getBounds()[0];
        require(bound.getRawType() == Carrier.class && bound.getActualTypeArguments()[0] instanceof WildcardType);
        require(get.getGenericReturnType().equals(formal) && get.getGenericParameterTypes()[0] == String.class);
        require(!Modifier.isFinal(get.getModifiers()));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Factory.events = 0;
            final FutureFactory factory = new FutureFactory();
            final Payload payload = mode == 10 ? null : new Payload();
            final Leaf initial = new Leaf(), alternate = new Leaf();
            initial.value = payload; alternate.value = new Payload();
            final Carrier<Bound> sibling = new Carrier<Bound>();
            sibling.value = new Bound();
            final Carrier<Bound> sentinel = new Carrier<Bound>();
            final Object marker = mode == 15 ? null : new Object();
            final RuntimeException callbackFailure = new RuntimeException();
            final RuntimeException targetFailure = new RuntimeException();
            final RuntimeException overrideFailure = new RuntimeException();
            final Error callbackError = new AssertionError("callback sentinel");
            final String key = mode == 12 ? null : new String(mode == 10 ? "\u0000key\ud800" : "key" + attempt);
            final int[] calls = {0};
            if (mode == 11) poison(initial, new Bound());
            factory.replacement = mode == 6 ? null : mode == 7 ? sibling : initial;
            factory.savedResult = sentinel; factory.returned = sentinel;
            factory.alternate = sibling; factory.afterFailure = overrideFailure;
            factory.failure = mode == 5 ? targetFailure : null;
            factory.probe = mode == 2 ? null : new Probe() {
                public Object observe(Factory owner, String actualKey) {
                    require(++calls[0] == 1 && owner == factory && actualKey == key);
                    require(factory.calls == 1 && factory.savedKey == key && Factory.events == 16);
                    require(factory.savedResult == sentinel && factory.returned == sentinel && factory.marker == null);
                    if (mode == 3) throw callbackFailure;
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
                if (route == 0) result = Core.call(supplied, key);
                else if (route == 1) Core.discard(supplied, key);
                else if (route == 2) result = Core.typed(supplied, key);
                else result = Core.narrowed(supplied, key);
            } catch (Throwable caught) { failure = caught; }
            boolean baseSuccess = mode != 1 && mode != 2 && mode != 3 && mode != 4 && mode != 5 && mode != 9;
            boolean success = baseSuccess && mode != 13;
            boolean narrowedFailure = success && route == 3 && (mode == 7 || mode == 14);
            int events = mode == 1 ? 3 : !baseSuccess ? 16 : mode == 13 ? 417 : route == 3 ? 10451 : 2089;
            require(Factory.events == events && factory.calls == (mode == 1 ? 0 : 1));
            require(calls[0] == (mode == 1 || mode == 2 ? 0 : 1));
            if (mode == 1 || mode == 2) require(failure instanceof NullPointerException);
            else if (mode == 3) require(failure == callbackFailure);
            else if (mode == 4) require(failure == callbackError);
            else if (mode == 5 || mode == 9) require(failure == targetFailure);
            else if (mode == 13) require(failure == overrideFailure);
            else if (narrowedFailure) require(failure instanceof ClassCastException);
            else require(failure == null);
            Carrier<?> cached = mode == 8 ? alternate : mode == 6 ? null : mode == 7 ? sibling : initial;
            Carrier<?> actual = mode == 14 ? sibling : cached;
            require(factory.savedKey == (mode == 1 ? null : key));
            require(factory.marker == (mode == 1 || mode == 2 || mode == 3 || mode == 4 ? null : marker));
            require(factory.savedResult == (baseSuccess ? cached : sentinel));
            require(factory.returned == (baseSuccess ? cached : sentinel));
            require(result == (success && !narrowedFailure && route != 1 ? actual : null));
            if (success && actual instanceof Leaf) {
                if (mode == 11) {
                    boolean bad = false;
                    try { Payload ignored = ((Leaf) actual).value; }
                    catch (ClassCastException caught) { bad = true; }
                    require(bad && Factory.events == events);
                } else require(((Leaf) actual).value == (mode == 8 ? alternate.value : payload));
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
