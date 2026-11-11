package fixture;
import java.lang.reflect.*;
public final class PackageNominalSelectorHarness {
    static int mode;
    static Object output;
    static RuntimeException failure;
    static void check(boolean v) { if (!v) throw new AssertionError("owned package nominal selector"); }
    static final class FutureProbe implements Probe {
        int calls, selected;
        public Object lookup(int index) {
            calls++; selected = index;
            if (mode == 3) throw failure;
            return output;
        }
    }
    public static void main(String[] args) throws Exception {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        Method method = Outer.Provider.class.getDeclaredMethod("select", int.class);
        TypeVariable<?> t = method.getTypeParameters()[0];
        check(t.getBounds()[0] == Object.class && method.getModifiers() == 0
            && ((ParameterizedType) method.getGenericReturnType()).getActualTypeArguments()[0].equals(t));
        for (int attempt = 0; attempt < 2; attempt++) {
            Outer.events = 0; failure = new RuntimeException("owned callback");
            RuntimeException providerFailure = new RuntimeException("owned provider");
            Outer.Provider owner = new Outer.Provider(); FutureProbe probe = new FutureProbe();
            Outer.Provider supplied = mode == 1 ? null : owner;
            owner.probe = mode == 2 ? null : probe;
            owner.failure = mode == 4 || mode == 6 ? providerFailure : null;
            int index = mode == 10 ? Integer.MIN_VALUE + attempt : mode == 11 ? Integer.MAX_VALUE - attempt : mode == 12 ? -1 : 17 + attempt;
            Object payload = mode == 7 ? null : mode == 8 ? new Object() : new String("payload" + attempt);
            Outer.Item cached = new Outer.Item(payload), sentinel = new Outer.Item(new Object());
            output = mode == 5 ? null : mode == 6 ? new Object() : cached;
            owner.result = sentinel; owner.selected = 99;
            Object result = null; Throwable caught = null;
            try {
                if (route == 0) result = Core.call(supplied, index);
                else if (route == 1) Core.discard(supplied, index);
                else if (route == 2) result = Core.<String>typed(supplied, index);
                else result = supplied.<String>select(index);
            } catch (Throwable error) { caught = error; }
            boolean success = mode != 1 && mode != 2 && mode != 3 && mode != 4 && mode != 6;
            if (success) check(caught == null);
            else if (mode == 1 || mode == 2) check(caught instanceof NullPointerException);
            else if (mode == 6) check(caught instanceof ClassCastException);
            else check(caught == (mode == 3 ? failure : providerFailure));
            int events = mode == 1 ? route == 3 ? 0 : 3 : route == 3 ? success ? 7 : 1 : success ? 414 : 16;
            check(Outer.events == events && probe.calls == (mode == 1 || mode == 2 ? 0 : 1));
            check(owner.selected == (mode == 1 ? 99 : index));
            check(probe.selected == (mode == 1 || mode == 2 ? 0 : index));
            check(owner.observed == (mode == 1 || mode == 2 || mode == 3 ? null : output));
            check(owner.result == (success ? output : sentinel));
            check(result == (success && route != 1 ? output : null));
            if (success && output != null) {
                Outer.Item<String> later = owner.result; String value = null; Throwable late = null;
                try { value = later.get(); } catch (Throwable error) { late = error; }
                check(mode == 8 ? late instanceof ClassCastException : late == null && value == payload);
                owner.result.value = new String("future"); String future = later.get();
                check(future == owner.result.value && Outer.events == (events * 5 + 4) * 5 + 4);
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
