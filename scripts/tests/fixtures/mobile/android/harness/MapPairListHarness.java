import fixture.*;
import fixture.lib.Facade;
import java.util.*;
import java.lang.reflect.*;

public final class MapPairListHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned map pair list invariant");
    }
    static final class FutureMap extends LinkedHashMap<String, Integer> {
        int reads;
        RuntimeException failure;
        Error error;
        public int size() {
            reads++;
            if (failure != null) throw failure;
            if (error != null) throw error;
            return super.size();
        }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        final int route = Integer.parseInt(args[0]);
        final int mode = Integer.parseInt(args[1]);
        Method method = Facade.class.getMethod("convert", Map.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        Type[] inputArguments = ((ParameterizedType) method.getGenericParameterTypes()[0]).getActualTypeArguments();
        ParameterizedType result = (ParameterizedType) method.getGenericReturnType();
        ParameterizedType pair = (ParameterizedType) result.getActualTypeArguments()[0];
        require(formals.length == 2 && result.getRawType() == List.class && pair.getRawType() == Pair.class);
        for (int i = 0; i < 2; ++i) {
            require(formals[i].getBounds()[0] == Object.class);
            require(((WildcardType) inputArguments[i]).getUpperBounds()[0].equals(formals[i]));
            require(pair.getActualTypeArguments()[i].equals(formals[i]));
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            State.events = 0;
            final FutureMap input = new FutureMap();
            input.put("input", attempt);
            if (mode == 15) input.put(null, null);
            input.reads = 0;
            final RuntimeException mapFailure = new RuntimeException();
            final RuntimeException callbackFailure = new RuntimeException();
            final RuntimeException targetFailure = new RuntimeException();
            final Error mapError = new AssertionError("map sentinel");
            final Error callbackError = new AssertionError("callback sentinel");
            input.failure = mode == 3 ? mapFailure : null;
            input.error = mode == 13 ? mapError : null;
            final Pair<String, Integer> good = new Pair<String, Integer>("key" + attempt, attempt);
            final Pair<String, Integer> changed = new Pair<String, Integer>("changed" + attempt, attempt + 1);
            final List alternate = Collections.singletonList(changed);
            final List initial = mode == 6 ? null : mode == 7 ? new ArrayList()
                    : mode == 8 ? Collections.singletonList(new Object())
                    : mode == 9 ? Collections.singletonList(new Pair<Integer, String>(1, "wrong"))
                    : Collections.singletonList(good);
            final List sentinel = new ArrayList();
            final Object oldMarker = new Object(), marker = mode == 14 ? null : new Object();
            final int[] calls = {0};
            State.savedInput = null; State.replacement = initial; State.savedResult = sentinel;
            State.marker = oldMarker;
            State.failure = mode == 5 ? targetFailure : null;
            State.probe = mode == 2 ? null : new Probe() {
                public Object observe(Object actualInput) {
                    require(++calls[0] == 1 && actualInput == input && State.savedInput == input);
                    require(input.reads == 1 && State.events == 16 && State.marker == oldMarker
                            && State.savedResult == sentinel);
                    if (mode == 4) throw callbackError;
                    if (mode == 11) throw callbackFailure;
                    if (mode == 10) {
                        input.put("future", 3);
                        State.replacement = alternate;
                    }
                    if (mode == 12) State.failure = targetFailure;
                    return marker;
                }
            };
            Object returned = null;
            Throwable failure = null;
            try {
                Map argument = mode == 1 ? null : input;
                if (route == 0) returned = Core.call(argument);
                else if (route == 1) returned = Core.typed(argument);
                else if (route == 2) Core.discard(argument);
                else returned = Core.head(argument);
            } catch (Throwable caught) { failure = caught; }
            boolean providerSuccess = mode != 1 && mode != 2 && mode != 3 && mode != 4
                    && mode != 5 && mode != 11 && mode != 12 && mode != 13;
            int events = providerSuccess ? (route == 3 ? 2076 : 414) : 16;
            require(State.events == events && input.reads == (mode == 1 ? 0 : 1));
            require(calls[0] == (mode == 1 || mode == 2 || mode == 3 || mode == 13 ? 0 : 1));
            if (mode == 1 || mode == 2) require(failure instanceof NullPointerException);
            else if (mode == 3) require(failure == mapFailure);
            else if (mode == 4) require(failure == callbackError);
            else if (mode == 5 || mode == 12) require(failure == targetFailure);
            else if (mode == 11) require(failure == callbackFailure);
            else if (mode == 13) require(failure == mapError);
            else if (route == 3 && mode == 6) require(failure instanceof NullPointerException);
            else if (route == 3 && mode == 7) require(failure instanceof IndexOutOfBoundsException);
            else if (route == 3 && mode == 8) require(failure instanceof ClassCastException);
            else require(failure == null);
            List expected = mode == 10 ? alternate : initial;
            require(State.savedInput == (mode == 1 ? null : input));
            require(State.marker == (mode == 1 || mode == 2 || mode == 3 || mode == 4
                    || mode == 11 || mode == 13 ? oldMarker : marker));
            require(State.savedResult == (providerSuccess ? expected : sentinel));
            if (providerSuccess) {
                if (route < 2) require(returned == expected);
                else if (route == 2 || mode == 6 || mode == 7 || mode == 8) require(returned == null);
                else require(returned == expected.get(0));
                if (expected != null && mode != 7) {
                    if (mode == 8 || mode == 9) {
                        boolean bad = false;
                        try { Pair<String, Integer> checked = (Pair<String, Integer>) expected.get(0);
                            String key = checked.first; Integer value = checked.second; }
                        catch (ClassCastException caught) { bad = true; }
                        require(bad && State.events == events);
                    } else {
                        Pair<String, Integer> checked = (Pair<String, Integer>) expected.get(0);
                        require(checked == (mode == 10 ? changed : good));
                    }
                }
            } else require(returned == null);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
