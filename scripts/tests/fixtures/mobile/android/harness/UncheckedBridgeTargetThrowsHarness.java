import fixture.*;
import java.lang.reflect.*;
import java.util.Arrays;

public final class UncheckedBridgeTargetThrowsHarness {
    private static int values, identities, states, failures;
    private static final Method TYPED;
    private static final Method BRIDGE;
    static {
        try {
            TYPED = Codec.class.getDeclaredMethod("convert", CharSequence.class);
            BRIDGE = Codec.class.getDeclaredMethod("convert", Object.class);
        } catch (ReflectiveOperationException error) { throw new AssertionError(error); }
    }
    private interface Call { Object run() throws Throwable; }
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned unchecked bridge contract");
    }
    private static Object invoke(Method method, Object receiver, Object value) throws Throwable {
        try { return method.invoke(receiver, value); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Object call(int path, Codec codec, CharSequence input) throws Throwable {
        switch (path) {
            case 0: return codec.convert(input);
            case 1: return ((Api<CharSequence, byte[]>) codec).convert(input);
            case 2: return ((Api) codec).convert(input);
            case 3: return invoke(TYPED, codec, input);
            case 4: return invoke(BRIDGE, codec, input);
            default: throw new AssertionError(path);
        }
    }
    private static void reset(int path, Codec codec) throws Throwable {
        Method method = (path == 1 || path == 4) ? Api.class.getDeclaredMethod("reset")
                        : path == 2 ? Codec.class.getMethod("reset")
                        : Codec.class.getDeclaredMethod("reset");
        try { method.invoke(codec); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void state(Codec codec, int expected) {
        require(codec.calls == expected);
        states++;
    }
    private static void thrown(Call call, Throwable expected, Class<?> type) throws Throwable {
        try { call.run(); throw new AssertionError("missing owned failure"); }
        catch (Throwable actual) {
            require(type.isInstance(actual));
            if (expected != null) { require(actual == expected); identities++; }
            failures++;
        }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        require(Arrays.equals(TYPED.getExceptionTypes(), new Class<?>[] {LeafFailure.class, FatalFailure.class}));
        require(BRIDGE.getExceptionTypes().length == 0);
        require(Api.class.getDeclaredMethod("convert", Object.class).getExceptionTypes().length == 0);
        require(!TYPED.isBridge() && !TYPED.isSynthetic() && BRIDGE.isBridge() && BRIDGE.isSynthetic());
        require(TYPED.getReturnType() == byte[].class && BRIDGE.getReturnType() == Object.class);
        require(TYPED.getTypeParameters().length == 0 && BRIDGE.getTypeParameters().length == 0);
        require(LeafFailure.class.getSuperclass() == MidFailure.class);
        require(MidFailure.class.getSuperclass() == RuntimeException.class);
        require(FatalFailure.class.getSuperclass() == Error.class);
        for (int path = 0; path < 5; path++) {
            final int p = path;
            for (byte[] result : new byte[][] {new byte[] {3, 7}, null}) {
                Codec codec = new Codec(result, new LeafFailure(), new FatalFailure());
                require(call(p, codec, new StringBuilder("owned")) == result);
                identities++; values++;
                state(codec, 1);
                if (result != null) { result[0] = 9; require(codec.result[0] == 9); }
            }
            Codec codec = new Codec(new byte[0], new LeafFailure(), new FatalFailure());
            thrown(() -> call(p, codec, null), codec.failure, LeafFailure.class);
            state(codec, 1);
            thrown(() -> call(p, codec, ""), codec.fatal, FatalFailure.class);
            state(codec, 2);
            thrown(() -> call(p, null, "owned"), null, NullPointerException.class);
            state(codec, 2);
        }
        Codec codec = new Codec(new byte[0], new LeafFailure(), new FatalFailure());
        thrown(() -> ((Api) codec).convert(Integer.valueOf(1)), null, ClassCastException.class);
        state(codec, 0);
        thrown(() -> invoke(BRIDGE, codec, Integer.valueOf(1)), null, ClassCastException.class);
        state(codec, 0);
        boolean defaults = args.length == 1 && args[0].equals("default");
        if (defaults) {
            Method reset = Codec.class.getDeclaredMethod("reset");
            require(Arrays.equals(reset.getExceptionTypes(), new Class<?>[] {LeafFailure.class, FatalFailure.class}));
            require(Api.class.getDeclaredMethod("reset").isDefault());
            require(Api.class.getDeclaredMethod("reset").getExceptionTypes().length == 0);
            require(!reset.isBridge() && !reset.isSynthetic() && reset.getReturnType() == void.class);
            for (int path = 0; path < 5; path++) {
                final int p = path;
                Codec target = new Codec(new byte[0], new LeafFailure(), new FatalFailure());
                thrown(() -> { reset(p, target); return null; }, target.failure, LeafFailure.class);
                state(target, 1);
                thrown(() -> { reset(p, target); return null; }, target.fatal, FatalFailure.class);
                state(target, 2);
                thrown(() -> { reset(p, null); return null; }, null, NullPointerException.class);
                state(target, 2);
            }
        }
        require(values == 10 && identities == (defaults ? 30 : 20)
                && states == (defaults ? 42 : 27) && failures == (defaults ? 32 : 17));
        System.out.println(defaults ? "values=10,identities=30,states=42,failures=32"
                                  : "values=10,identities=20,states=27,failures=17");
        System.out.println("unchecked-target-exceptions=retained,erased-interface-exceptions=empty,casts-before-effects=retained");
    }
}
