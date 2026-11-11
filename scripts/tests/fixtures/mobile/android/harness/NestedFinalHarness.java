import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;

public final class NestedFinalHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("nested final initialization changed"); }
    static final class Supplied implements Delegate {
        public Object read() { Trace.hit(15); return Trace.payload; }
    }
    static final class Factory implements Provider {
        public Delegate get() { Trace.hit(10); return Trace.provided; }
    }
    static Core create(int route, Object activity, Object toolbar, Object supplied,
                       int first, int second, long stamp) throws Throwable {
        if (route == 0) return new Core(activity, toolbar, supplied, first, second, stamp);
        if (route == 1) return NestedFinalCaller.create(activity, toolbar, supplied, first, second, stamp);
        if (route == 2) {
            try { return Core.class.getConstructor(Object.class, Object.class, Object.class, int.class, int.class, long.class)
                .newInstance(activity, toolbar, supplied, first, second, stamp); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Core) MethodHandles.publicLookup().findConstructor(Core.class,
            MethodType.methodType(void.class, Object.class, Object.class, Object.class, int.class, int.class, long.class))
            .invokeExact(activity, toolbar, supplied, first, second, stamp);
    }
    public static void main(String[] args) throws Throwable {
        int branch = Integer.parseInt(args[0]), suppliedMode = Integer.parseInt(args[1]);
        int route = Integer.parseInt(args[2]), mode = Integer.parseInt(args[3]);
        for (String name : new String[]{"selected", "first", "second", "stamp"})
            require(Modifier.isFinal(Core.class.getField(name).getModifiers()));
        Object activity = branch == 1 ? new Factory() : new Object();
        Object toolbar = branch == 0 ? new Object() : null;
        Object supplied = suppliedMode == 1 ? activity : null;
        int first = -2147483647, second = 2147483646;
        long stamp = 0x8123456789abcdefL;
        Trace.provided = mode == 16 ? null : new Supplied();
        Trace.payload = mode == 17 ? null : toolbar != null ? toolbar : activity;
        Trace.failure = new RuntimeException("owned nested initialization failure");
        Trace.failAt = mode <= 15 ? mode : 0;
        Set<Integer> initialized = new HashSet<Integer>(), failed = new HashSet<Integer>();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.core = null; Trace.allocated = null; Trace.token = null;
            boolean[] seen = Trace.seen = new boolean[7];
            Object[] selectedAt = Trace.selectedAt = new Object[7], extraAt = Trace.extraAt = new Object[7];
            int[] firstAt = Trace.firstAt = new int[7], secondAt = Trace.secondAt = new int[7];
            long[] stampAt = Trace.stampAt = new long[7];
            Throwable failure = null; Core result = null;
            try { result = create(route, activity, toolbar, supplied, first, second, stamp); }
            catch (Throwable problem) { failure = problem; }
            List<Integer> stages = new ArrayList<Integer>();
            stages.add(1);
            if (branch == 0) stages.addAll(Arrays.asList(7, 12, 2));
            else if (branch == 1) stages.addAll(Arrays.asList(10, 3));
            else stages.addAll(Arrays.asList(8, 13, 4));
            stages.add(5);
            if (supplied == null) stages.addAll(Arrays.asList(9, branch == 1 && mode == 16 ? -1 : branch == 1 ? 15 : 11, 14));
            stages.add(6);
            long trace = 0;
            int failureKind = 0;
            Object selected = null, extra = null;
            int expectedFirst = 0, expectedSecond = 0;
            long expectedStamp = 0;
            boolean[] expectedSeen = new boolean[7];
            boolean primaryComplete = false, alternateComplete = false, tokenComplete = false;
            for (int stage : stages) {
                if (stage == -1) { failureKind = 4; break; }
                boolean init = stage == 7 || stage == 8 || stage == 9;
                if (init && failed.contains(stage)) { failureKind = 3; break; }
                if (init && initialized.contains(stage)) continue;
                if (stage == 2 || stage == 4) selected = Trace.allocated;
                if (stage == 3) selected = Trace.provided;
                if (stage == 5) { expectedFirst = first; expectedSecond = second; expectedStamp = stamp; }
                if (stage == 6) extra = supplied != null ? supplied : Trace.token;
                if (stage >= 1 && stage <= 6) {
                    expectedSeen[stage] = true;
                    require(selectedAt[stage] == selected && extraAt[stage] == extra
                        && firstAt[stage] == expectedFirst && secondAt[stage] == expectedSecond && stampAt[stage] == expectedStamp);
                }
                trace = trace * 17 + stage;
                if (Trace.failAt == stage) {
                    failureKind = init ? 2 : 1;
                    if (init) failed.add(stage);
                    break;
                }
                if (init) initialized.add(stage);
                primaryComplete |= stage == 12;
                alternateComplete |= stage == 13;
                tokenComplete |= stage == 14;
            }
            require(Trace.events == trace && Arrays.equals(seen, expectedSeen)
                && Trace.seen == seen && Trace.selectedAt == selectedAt && Trace.extraAt == extraAt
                && Trace.firstAt == firstAt && Trace.secondAt == secondAt && Trace.stampAt == stampAt);
            require(Trace.core != null && Trace.core.selected == selected && Trace.core.first == expectedFirst
                && Trace.core.second == expectedSecond && Trace.core.stamp == expectedStamp && Trace.core.extra == extra);
            if (Trace.allocated instanceof Primary)
                require(((Primary) Trace.allocated).marker == (primaryComplete ? toolbar : null));
            if (Trace.allocated instanceof Alternate)
                require(((Alternate) Trace.allocated).marker == activity);
            if (Trace.token != null) require(((Token) Trace.token).value == (tokenComplete ? Trace.payload : null));
            if (failureKind == 1) require(failure == Trace.failure);
            else if (failureKind == 2) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
            else if (failureKind == 3) require(failure instanceof NoClassDefFoundError);
            else if (failureKind == 4) require(failure instanceof NullPointerException);
            else require(failure == null && result == Trace.core && result.selected == selected && result.extra == extra);
            System.out.println("branch=" + branch + ",supplied=" + suppliedMode + ",route=" + route + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
