import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
public final class SharedElseFinalHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("shared else final contract"); }
    static Core create(int route, Value value, int number) throws Throwable {
        if (route == 0) return new Core(value, number);
        if (route == 1) return SharedElseFinalCaller.create(value, number);
        if (route == 2) {
            try { return Core.class.getConstructor(Value.class, int.class).newInstance(value, number); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Core) MethodHandles.publicLookup().findConstructor(Core.class,
            MethodType.methodType(void.class, Value.class, int.class)).invokeExact(value, number);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        boolean first = variant < 16 ? (variant & 1) != 0 : variant != 21;
        boolean second = variant < 16 ? (variant & 4) != 0 : true;
        Value value = variant < 16 && (variant & 8) != 0 ? null
            : new Value(variant < 16 && (variant & 2) == 0 ? 0 : Integer.MIN_VALUE);
        int[] failures = {1, 2, 3, 7, 4, 5, 6};
        Trace.first = first; Trace.second = second;
        Trace.failAt = variant >= 16 && variant <= 22 ? failures[variant - 16] : 0;
        Trace.failure = new IllegalStateException("owned shared else failure");
        Trace.chosen = variant == 23 ? null : new Object();
        require(Modifier.isFinal(Core.class.getField("selected").getModifiers()));
        require(Modifier.isFinal(Core.class.getField("number").getModifiers()));
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.core = null;
            Trace.seen = new boolean[8]; Trace.selectedAt = new Object[8]; Trace.numberAt = new int[8];
            int number = attempt == 0 ? Integer.MIN_VALUE : Integer.MAX_VALUE;
            List<Integer> stages = new ArrayList<Integer>(Arrays.asList(1, 2));
            boolean nullFailure = first && value == null;
            boolean chosen = first && value != null && value.number != 0 && second;
            if (nullFailure) stages.add(-1);
            else {
                if (first && value.number != 0) stages.add(3);
                if (chosen) stages.add(7);
                stages.add(chosen ? 4 : 5); stages.add(6);
            }
            Object selected = null; int expectedNumber = 0;
            long events = 0; boolean expectedFailure = false, expectedNull = false;
            boolean[] seen = new boolean[8];
            for (int stage : stages) {
                if (stage == -1) { expectedNull = true; break; }
                if (stage == 4) selected = Trace.chosen;
                if (stage == 5) selected = value;
                if (stage == 6) expectedNumber = number;
                if (stage == 1 || stage == 4 || stage == 5 || stage == 6) seen[stage] = true;
                events = events * 17 + stage;
                if (stage == Trace.failAt) { expectedFailure = true; break; }
            }
            Throwable caught = null; Core result = null;
            try { result = create(route, value, number); }
            catch (Throwable actual) { caught = actual; }
            require(expectedFailure ? caught == Trace.failure : expectedNull ? caught instanceof NullPointerException : caught == null);
            require((caught == null ? result == Trace.core : result == null) && Trace.core != null);
            require(Trace.events == events && Arrays.equals(Trace.seen, seen)
                && Trace.core.selected == selected && Trace.core.number == expectedNumber);
            for (int stage : new int[] {1, 4, 5, 6}) if (seen[stage]) {
                Object at = stage == 1 ? null : chosen ? Trace.chosen : value;
                require(Trace.selectedAt[stage] == at && Trace.numberAt[stage] == (stage == 6 ? number : 0));
            }
        }
        System.out.println("variant=" + variant + ",route=" + route + ",ok");
    }
}
