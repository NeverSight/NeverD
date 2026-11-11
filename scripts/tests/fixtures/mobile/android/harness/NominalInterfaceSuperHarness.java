package probe;
import fixture.*;
import java.io.IOException;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class NominalInterfaceSuperHarness {
    static void check(boolean value) {
        if (!value) throw new AssertionError("owned nominal interface super mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    static final class Future extends Receiver {
        @Override public int transform(int value) {
            Trace.hit(7);
            int result = super.transform(value);
            Trace.hit(8);
            return result ^ 37;
        }
    }
    static Output call(int route, Owner owner, Input value) throws Throwable {
        if (route == 0) return Core.call(owner, value);
        if (route == 1) return NominalInterfaceSuperCaller.call(owner, value);
        Trace.hit(6);
        Output result;
        try {
            if (route == 2) result = (Output) Root.class.getMethod("convert", Input.class).invoke(owner, value);
            else if (route == 3) {
                MethodHandle handle = MethodHandles.publicLookup().findVirtual(Root.class, "convert", MethodType.methodType(Output.class, Input.class));
                result = (Output) handle.invokeExact((Root) owner, value);
            } else if (route == 4) result = (Output) Owner.class.getMethod("convert", Input.class).invoke(owner, value);
            else if (route == 5) {
                MethodHandle handle = MethodHandles.publicLookup().findVirtual(Owner.class, "convert", MethodType.methodType(Output.class, Input.class));
                result = (Output) handle.invokeExact(owner, value);
            } else result = NominalInterfaceSuperCaller.direct(owner, value);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
        Trace.hit(0);
        return result;
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean future = Boolean.parseBoolean(args[2]), bridge = Boolean.parseBoolean(args[3]);
        Method entry = Owner.class.getDeclaredMethod("convert", Input.class);
        check(entry.isBridge() == bridge && !entry.isSynthetic() && Modifier.isPublic(entry.getModifiers()));
        check(entry.getReturnType() == Output.class && entry.getTypeParameters().length == 0 && entry.getExceptionTypes().length == 0);
        check(Root.class.getMethod("convert", Input.class).isDefault());
        check(java.util.Arrays.equals(Owner.class.getInterfaces(), new Class<?>[]{Api.class, Side.class}));
        check(java.util.Arrays.equals(Api.class.getInterfaces(), new Class<?>[]{Root.class, Side.class}));
        check(java.util.Arrays.equals(Root.class.getInterfaces(), new Class<?>[]{Leaf.class}));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Trace.beginAction = Trace.readAction = Trace.transformAction = Trace.outputAction = Trace.afterAction = null;
            Object sentinel = new Object();
            Input sentinelInput = new Input(123);
            Output sentinelOutput = new Output(456);
            int bits = (attempt == 0 ? 0x7fc12345 : 0x80000000) ^ mode;
            Input value = mode == 1 ? null : new Input(bits);
            Owner owner = mode == 9 ? null : future ? new Future() : new Receiver();
            Receiver other = new Receiver();
            Input nestedInput = new Input(~bits);
            Output[] nested = {null};
            Trace.events = 0;
            Trace.outputCalls = 0;
            Trace.mask = 0x6dbe1729;
            Trace.parent = Trace.transformOwner = sentinel;
            Trace.input = Trace.readOwner = sentinelInput;
            Trace.constructed = Trace.result = sentinelOutput;
            int[] calls = {0};
            Throwable failure = mode == 2 || mode == 5 || mode == 10 ? new IOException("owned checked")
                : mode == 4 || mode == 11 ? new AssertionError("owned error") : new IllegalStateException("owned runtime");
            Runnable action = () -> { calls[0]++; raise(failure); };
            if (mode == 2) Trace.beginAction = action;
            if (mode == 3 || mode == 10) Trace.readAction = action;
            if (mode == 4) Trace.transformAction = action;
            if (mode == 5) Trace.outputAction = action;
            if (mode == 6 || mode == 11) Trace.afterAction = action;
            if (mode == 7) Trace.beginAction = () -> { calls[0]++; value.bits ^= 0x5a; Trace.mask = 0x33557799; };
            if (mode == 8) Trace.beginAction = () -> {
                calls[0]++;
                Trace.beginAction = null;
                nested[0] = Core.call(other, nestedInput);
                check(nested[0].bits == (~bits ^ 0x6dbe1729));
            };
            Output result = null;
            Throwable caught = null;
            try { result = call(route, owner, value); } catch (Throwable error) { caught = error; }
            boolean begun = mode != 9, read = begun && mode != 1 && mode != 2;
            boolean transform = read && mode != 3 && mode != 10;
            boolean output = transform && mode != 4, after = output && mode != 5;
            boolean tail = after && mode != 6 && mode != 11;
            String events = "6";
            if (begun) events += "1";
            if (mode == 8) events += "6123450";
            if (read) events += "2";
            if (transform) events += future ? "73" : "3";
            if (output && future) events += "8";
            if (output) events += "4";
            if (after) events += "5";
            if (tail) events += "0";
            check(Trace.events == Long.parseLong(events));
            check(Trace.parent == (begun ? mode == 8 ? other : owner : sentinel));
            check(Trace.input == (begun ? mode == 8 ? nestedInput : value : sentinelInput));
            check(Trace.readOwner == (read ? value : sentinelInput));
            check(Trace.transformOwner == (transform ? owner : sentinel));
            check(Trace.outputCalls == (output ? 1 : 0) + (mode == 8 ? 1 : 0));
            check(calls[0] == (mode == 0 || mode == 1 || mode == 9 ? 0 : 1));
            int expected = (bits ^ (mode == 7 ? 0x5a : 0)) ^ (mode == 7 ? 0x33557799 : 0x6dbe1729) ^ (future ? 37 : 0);
            check(output ? Trace.constructed != sentinelOutput && Trace.constructed.bits == expected : Trace.constructed == sentinelOutput);
            check(Trace.result == (after ? Trace.constructed : sentinelOutput));
            if (tail) check(caught == null && result == Trace.result && result != nested[0]);
            else if (mode == 1 || mode == 9) check(caught instanceof NullPointerException && result == null);
            else check(caught == failure && result == null);
            if (mode == 8) check(nested[0] != null && nested[0] != Trace.constructed);
        }
        System.out.println("attempts=2,nominal-interface-super=matched");
    }
}
