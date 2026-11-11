import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public class AnchoredVoidCarrierHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("owned anchored void contract"); }
    static void call(int route, Cell<Object> cell, Callback<Object,Boolean> callback,
                     Callback<String,Boolean> fixed) throws Throwable {
        if (route == 0) Core.call(cell, callback, fixed, 37);
        else if (route == 1) AnchoredVoidCarrierCaller.call(cell, callback, fixed, 37);
        else if (route == 2) {
            try { Core.class.getMethod("call", Cell.class, Callback.class, Callback.class, int.class)
                    .invoke(null, cell, callback, fixed, 37); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        } else MethodHandles.publicLookup().findStatic(Core.class, "call",
                MethodType.methodType(void.class, Cell.class, Callback.class, Callback.class, int.class))
                .invoke(cell, callback, fixed, 37);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        require(Provider.class.getTypeParameters().length == 1);
        Trace.failure = new IllegalArgumentException("owned callback failure");
        Trace.fail = mode <= 5 ? mode : 0;
        int[] data = {7}, firstCalls = {0}, fixedCalls = {0};
        Object input = mode == 9 ? null : data;
        Cell<Object> owned = new Cell<Object>(input);
        Cell<Object> cell = mode == 6 ? null : owned;
        Callback<Object,Boolean> callback = mode == 7 ? null : value -> {
            require(value == input); firstCalls[0]++; Trace.hit(2);
            if (value != null) ((int[])value)[0]++;
            return mode == 10 ? null : Boolean.TRUE;
        };
        Callback<String,Boolean> fixed = mode == 8 ? null : value -> {
            require(value.equals("owned")); fixedCalls[0]++; Trace.hit(4);
            return mode == 11 ? null : Boolean.TRUE;
        };
        Trace.expectedCell = cell; Trace.expectedCallback = callback;
        Trace.expectedFixed = fixed; Trace.expectedInput = input;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0;
            int oldData = data[0], oldFirst = firstCalls[0], oldFixed = fixedCalls[0];
            Throwable thrown = null;
            try { call(route, cell, callback, fixed); } catch (Throwable failure) { thrown = failure; }
            boolean first = mode != 1 && mode != 6 && mode != 7;
            boolean second = first && mode != 2 && mode != 3 && mode != 8 && mode != 10;
            long expected = 1;
            if (first) expected = expected * 10 + 2;
            if (first && mode != 2 && mode != 10) expected = expected * 10 + 3;
            if (second) expected = expected * 10 + 4;
            if (second && mode != 4 && mode != 11) expected = expected * 10 + 5;
            if (mode == 0 || mode == 9) require(thrown == null);
            else if (mode >= 1 && mode <= 5) require(thrown == Trace.failure);
            else require(thrown instanceof NullPointerException);
            require(Trace.events == expected && firstCalls[0] == oldFirst + (first ? 1 : 0));
            require(fixedCalls[0] == oldFixed + (second ? 1 : 0));
            require(data[0] == oldData + (first && mode != 2 && input != null ? 1 : 0));
            require(owned.value == input);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
