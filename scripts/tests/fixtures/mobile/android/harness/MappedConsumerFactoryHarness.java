import fixture.Core;
import fixture.Mapper;
import fixture.Rule;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.Comparator;
import java.util.function.Function;

public class MappedConsumerFactoryHarness {
    static void check(boolean condition) {
        if (!condition) throw new AssertionError("owned mapped consumer factory invariant");
    }

    @SuppressWarnings("unchecked")
    static <T extends Throwable> void raise(Throwable error) throws T { throw (T) error; }

    static Rule owned(int route, Core core) throws Throwable {
        if (route == 0) return core.run();
        if (route == 1) return MappedConsumerFactoryCaller.owned(core);
        if (route == 2) {
            try { return (Rule) Core.class.getMethod("run").invoke(core); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Rule) MethodHandles.lookup().findVirtual(Core.class, "run",
            MethodType.methodType(Rule.class)).invokeExact(core);
    }

    static Comparator jdk(int route, Core core, Function mapper, Comparator comparator) throws Throwable {
        if (route == 0) return core.jdk(mapper, comparator);
        if (route == 1) return MappedConsumerFactoryCaller.jdk(core, mapper, comparator);
        if (route == 2) {
            try { return (Comparator) Core.class.getMethod("jdk", Function.class, Comparator.class)
                .invoke(core, mapper, comparator); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Comparator) MethodHandles.lookup().findVirtual(Core.class, "jdk",
            MethodType.methodType(Comparator.class, Function.class, Comparator.class))
            .invokeExact(core, mapper, comparator);
    }

    static int ownedCase(final int route, final int mode, int attempt) throws Throwable {
        final Core core = new Core();
        final int[] mappings = {0}, actions = {0};
        final Mapper<Object, Object> mapper = new Mapper<Object, Object>() {
            public Object map(Object value) { ++mappings[0]; return value; }
        };
        final MappedConsumerFactoryFuture input = new MappedConsumerFactoryFuture();
        final MappedConsumerFactoryFuture initial = new MappedConsumerFactoryFuture();
        final MappedConsumerFactoryFuture changed = new MappedConsumerFactoryFuture();
        initial.answer = Integer.MIN_VALUE + attempt;
        changed.answer = 0x7135abcf + attempt;
        final Throwable failure = mode == 5 || mode == 8 ? new IllegalStateException("owned failure")
            : mode == 6 || mode == 9 ? new AssertionError("owned failure") : new Exception("owned failure");
        if (mode >= 7 && mode <= 9) initial.failure = failure;
        core.mapper = mode == 1 ? null : mapper;
        core.rule = mode == 2 ? null : input;
        final Mapper originalMapper = core.mapper;
        final Rule originalRule = core.rule;
        Trace.events = 0;
        Trace.mapper = null;
        Trace.rule = null;
        Trace.result = mode == 3 ? null : initial;
        Trace.action = new Runnable() {
            public void run() {
                ++actions[0];
                check(actions[0] == 1 && Trace.events == 12 && mappings[0] == 0 && input.calls == 0);
                check(Trace.mapper == originalMapper && Trace.rule == originalRule);
                if (mode == 12) {
                    core.mapper = null;
                    core.rule = changed;
                    Trace.result = changed;
                }
                if (mode == 13) Trace.result = null;
                if (mode >= 4 && mode <= 6) MappedConsumerFactoryHarness.<RuntimeException>raise(failure);
            }
        };
        Rule result = null;
        Throwable caught = null;
        try { result = owned(route, core); }
        catch (Throwable error) { caught = error; }
        check(actions[0] == 1 && mappings[0] == 0 && input.calls == 0);
        check(Trace.mapper == originalMapper && Trace.rule == originalRule);
        if (mode >= 4 && mode <= 6) {
            check(caught == failure && result == null && Trace.events == 12);
            return Trace.events;
        }
        MappedConsumerFactoryFuture expected = mode == 3 || mode == 13 ? null : mode == 12 ? changed : initial;
        check(caught == null && result == expected && Trace.events == 123);
        if (result == null) return Trace.events;
        Object first = mode == 10 ? new Object() : mode == 1 ? null : "first";
        Object second = mode == 11 ? new Object() : mode == 2 ? null : "second";
        int value = 0;
        try { value = result.compare(first, second); }
        catch (Throwable error) { caught = error; }
        if (mode == 10 || mode == 11) check(caught instanceof ClassCastException && expected.calls == 0);
        else {
            check(expected.calls == 1 && expected.first == first && expected.second == second);
            if (mode >= 7 && mode <= 9) check(caught == failure);
            else check(caught == null && value == expected.answer);
        }
        check(mappings[0] == 0 && input.calls == 0 && Trace.events == 123);
        return Trace.events;
    }

    static int jdkCase(final int route, final int mode) throws Throwable {
        final Core core = new Core();
        final int[] events = {0}, maps = {0}, comparisons = {0};
        final Object[] mapped = new Object[2];
        final Throwable failure = mode == 5 || mode == 8 ? new IllegalStateException("owned JDK failure")
            : mode == 6 || mode == 9 ? new AssertionError("owned JDK failure") : new Exception("owned JDK failure");
        final String left = "alpha", right = mode == 13 ? left : "pi";
        Function<String, Integer> mapper = new Function<String, Integer>() {
            public Integer apply(String value) {
                check(maps[0] < 2);
                mapped[maps[0]++] = value;
                events[0] = events[0] * 10 + 1;
                if ((maps[0] == 1 && (mode == 4 || mode == 6)) || (maps[0] == 2 && mode == 5))
                    MappedConsumerFactoryHarness.<RuntimeException>raise(failure);
                return Integer.valueOf(value.length());
            }
        };
        Comparator<Integer> consumer = new Comparator<Integer>() {
            public int compare(Integer first, Integer second) {
                ++comparisons[0];
                events[0] = events[0] * 10 + 2;
                check(maps[0] == 2 && comparisons[0] == 1 && first.intValue() == left.length()
                    && second.intValue() == right.length());
                if (mode >= 7 && mode <= 9) MappedConsumerFactoryHarness.<RuntimeException>raise(failure);
                return -700 + first.intValue() * 31 - second.intValue() * 7;
            }
        };
        Function selectedMapper = mode == 1 ? null : mapper;
        Comparator selectedConsumer = mode == 2 ? null : consumer;
        Comparator result = null;
        Throwable caught = null;
        try { result = jdk(route, core, selectedMapper, selectedConsumer); }
        catch (Throwable error) { caught = error; }
        check(events[0] == 0 && maps[0] == 0 && comparisons[0] == 0);
        if (mode == 1 || mode == 2) {
            check(caught instanceof NullPointerException && result == null);
            return events[0];
        }
        check(caught == null && result != null);
        Object first = mode == 3 ? null : mode == 10 ? new Object() : left;
        Object second = mode == 12 ? null : mode == 11 ? new Object() : right;
        int value = 0;
        try { value = result.compare(first, second); }
        catch (Throwable error) { caught = error; }
        int expectedMaps = mode == 10 ? 0 : mode == 3 || mode == 4 || mode == 6 || mode == 11 ? 1 : 2;
        int expectedComparisons = mode == 0 || mode == 13 || (mode >= 7 && mode <= 9) ? 1 : 0;
        int expectedEvents = expectedComparisons == 1 ? 112 : expectedMaps == 2 ? 11 : expectedMaps;
        check(maps[0] == expectedMaps && comparisons[0] == expectedComparisons && events[0] == expectedEvents);
        if (expectedMaps >= 1) check(mapped[0] == first);
        if (expectedMaps >= 2) check(mapped[1] == second);
        if (mode == 10 || mode == 11) check(caught instanceof ClassCastException);
        else if (mode == 3 || mode == 12) check(caught instanceof NullPointerException);
        else if (mode >= 4 && mode <= 9) check(caught == failure);
        else check(caught == null && value == -700 + left.length() * 31 - right.length() * 7);
        return events[0];
    }

    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]);
        String platform = args[1];
        int mode = Integer.parseInt(args[2]);
        for (int attempt = 0; attempt != 2; ++attempt) {
            int events = platform.equals("jdk") ? jdkCase(route, mode) : ownedCase(route, mode, attempt);
            System.out.println("route=" + route + ",platform=" + platform + ",mode=" + mode
                + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
