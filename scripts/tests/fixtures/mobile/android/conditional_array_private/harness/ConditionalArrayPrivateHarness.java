import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ConditionalArrayPrivateHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned conditional array private contract");
    }
    private static int[] numbers(int first, int second) { return new int[]{first, second}; }
    private static int[] emptyValues() { return new int[0]; }
    private static Target make(int route, Core receiver, int index) throws Throwable {
        if (route == 0) return receiver.make(index);
        if (route == 1) return ConditionalArrayPrivateCaller.make(receiver, index);
        if (route == 2) {
            try { return (Target) Core.class.getMethod("make", int.class).invoke(receiver, index); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Target) MethodHandles.lookup().findVirtual(Core.class, "make",
            MethodType.methodType(Target.class, int.class)).invokeWithArguments(receiver, index);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), variant = Integer.parseInt(args[1]);
        int fail = Integer.parseInt(args[2]), kind = Integer.parseInt(args[3]);
        Trace.failure = kind == 0 ? new IllegalArgumentException("owned conditional failure")
            : new LinkageError("owned conditional failure");
        Trace.failAt = fail;
        for (int attempt = 0; attempt < 2; attempt++) {
            Core receiver = new Core();
            int[] initialValues = variant == 3 ? null : variant == 4 ? emptyValues() : numbers(101, 202);
            int[] nextValues = variant == 9 || variant == 11 ? null : variant == 10 ? emptyValues()
                : variant == 13 ? initialValues : numbers(303, 404);
            int[] initialFlags = new int[]{9, 9};
            int[] nextFlags = variant == 7 ? null : variant == 8 ? new int[0]
                : new int[]{variant == 1 || variant == 9 || variant == 10 || variant == 13 ? 0
                    : variant == 2 ? -7 : 5, -7};
            int index = variant == 5 ? -1 : variant == 6 ? 2 : variant == 15 ? 1 : 0;
            Object key = variant == 12 ? null : new Object();
            Trace.replacement = variant == 14 ? key : new Object();
            Trace.replacementValue = 991;
            Trace.afterValues = nextValues;
            Trace.afterFlags = nextFlags;
            receiver.values = initialValues;
            receiver.flags = initialFlags;
            receiver.token = key;
            Trace.events = Trace.mask = 0;
            Trace.seenIndex = -99;
            Trace.seenNext = 0;
            Trace.seenFirst = Trace.seenBranch = 0;
            Trace.seenArray = Trace.seenKey = Trace.seenReturn = null;
            Trace.seenTarget = Trace.result = null;
            boolean initFailure = fail == 8;
            boolean initialBad = initialValues == null || index < 0 || index >= initialValues.length;
            boolean flagsBad = nextFlags == null || index < 0 || index >= nextFlags.length;
            boolean postBad = flagsBad
                || nextFlags[index] <= 0 && (nextValues == null || index >= nextValues.length);
            long expectedFirst = initialBad ? 0 : initialValues[index];
            long expectedBranch = postBad ? 0 : nextFlags[index] > 0 ? nextFlags[index]
                : variant == 13 ? Trace.replacementValue : nextValues[index];
            int expectedEvents = attempt == 0 ? 8 : 0, expectedMask = 0;
            boolean privateEntered = !initFailure && !initialBad;
            boolean mutated = privateEntered && fail != 1;
            boolean returned = mutated && fail != 2;
            boolean constructed = returned && !postBad;
            if (privateEntered) {
                expectedEvents = expectedEvents * 10 + 1;
                if (mutated) {
                    expectedEvents = expectedEvents * 10 + 2;
                    expectedMask = 1;
                    if (constructed) {
                        expectedEvents = expectedEvents * 10 + 3;
                        expectedMask = 3;
                        if (fail != 3) { expectedEvents = expectedEvents * 10 + 4; expectedMask = 7; }
                    }
                }
            }
            Target result = null;
            Throwable caught = null;
            try { result = make(route, receiver, index); } catch (Throwable error) { caught = error; }
            require(Trace.events == expectedEvents && Trace.mask == expectedMask);
            if (initFailure) {
                if (attempt == 1) require(caught instanceof NoClassDefFoundError);
                else if (kind == 0) require(caught instanceof ExceptionInInitializerError
                    && ((ExceptionInInitializerError) caught).getException() == Trace.failure);
                else require(caught == Trace.failure);
            } else if (initialBad || returned && postBad) {
                boolean isNull = initialBad ? initialValues == null : nextFlags == null
                    || !flagsBad && nextFlags[index] <= 0 && nextValues == null;
                require(isNull ? caught instanceof NullPointerException : caught instanceof ArrayIndexOutOfBoundsException);
            } else require(fail == 0 ? caught == null && result == Trace.seenTarget && result == Trace.result
                : caught == Trace.failure);
            require(receiver.values == (mutated ? nextValues : initialValues)
                && receiver.flags == (mutated ? nextFlags : initialFlags)
                && receiver.token == (mutated ? Trace.replacement : key));
            require(Trace.seenArray == (privateEntered ? initialValues : null)
                && Trace.seenIndex == (privateEntered ? index : -99)
                && Trace.seenReturn == (returned ? key : null));
            if (mutated) require(initialValues[index] == Trace.replacementValue);
            if (constructed) {
                require(Trace.seenTarget != null && Trace.seenFirst == expectedFirst && Trace.seenKey == key
                    && Trace.seenBranch == expectedBranch && Trace.seenNext == index + 1);
                if (expectedMask == 3) require(Trace.seenTarget.first == 0 && Trace.seenTarget.key == null
                    && Trace.seenTarget.branch == 0 && Trace.seenTarget.next == 0);
                else require(Trace.seenTarget.first == expectedFirst && Trace.seenTarget.key == key
                    && Trace.seenTarget.branch == expectedBranch && Trace.seenTarget.next == index + 1);
            } else require(Trace.seenTarget == null && Trace.seenFirst == 0 && Trace.seenKey == null
                && Trace.seenBranch == 0 && Trace.seenNext == 0);
            if (fail != 0 || !constructed) require(result == null && Trace.result == null);
            System.out.println("route=" + route + ",variant=" + variant + ",fail=" + fail + ",kind=" + kind
                + ",attempt=" + attempt + ",events=" + Trace.events + ",mask=" + Trace.mask);
        }
    }
}
