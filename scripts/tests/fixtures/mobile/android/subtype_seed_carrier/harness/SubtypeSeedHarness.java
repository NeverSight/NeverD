import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class SubtypeSeedHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned subtype seed contract");
    }
    static void raise(Throwable failure) {
        if (failure instanceof RuntimeException) throw (RuntimeException) failure;
        throw (Error) failure;
    }
    static final class Input implements Producer<Object> {
        Object item;
        int reads;
        Input(Object item) { this.item = item; }
        public Object value() { Object captured = item; reads++; Trace.hit(5); return captured; }
    }
    static final class Output implements Result<Object> {
        int reads;
        public Object value() { reads++; throw new AssertionError("opaque result was evaluated"); }
    }
    static final class Receiver implements Context {
        Object value, seed;
        long flags;
        int bits, calls;
        public void accept(Object value, Object seed, long flags, int bits) {
            this.value = value; this.seed = seed; this.flags = flags; this.bits = bits;
            calls++; Trace.hit(6);
        }
    }
    static void metadata() throws Exception {
        Method method = Provider.class.getMethod("select", Producer.class, Object.class, Context.class, long.class, int.class);
        TypeVariable<Method>[] variables = method.getTypeParameters();
        require(variables.length == 2 && variables[0].getName().equals("T") && variables[1].getName().equals("R"));
        require(variables[0].getBounds().length == 1 && variables[0].getBounds()[0].equals(variables[1]));
        require(variables[1].getBounds().length == 1 && variables[1].getBounds()[0] == Object.class);
        ParameterizedType input = (ParameterizedType) method.getGenericParameterTypes()[0];
        require(input.getRawType() == Producer.class && ((WildcardType) input.getActualTypeArguments()[0]).getUpperBounds()[0].equals(variables[0]));
        require(method.getGenericParameterTypes()[1].equals(variables[1]));
        ParameterizedType result = (ParameterizedType) method.getGenericReturnType();
        require(result.getRawType() == Result.class && result.getActualTypeArguments()[0].equals(variables[1]));
    }
    static Result call(int route, Producer source, Object seed, Context context, long flags, int bits) throws Throwable {
        try {
            if (route == 0) return Client.pick(source, seed, context, flags, bits);
            if (route == 1) return SubtypeSeedCaller.pick(source, seed, context, flags, bits);
            if (route == 2) return (Result) Client.class.getMethod("pick", Producer.class, Object.class, Context.class, long.class, int.class)
                .invoke(null, source, seed, context, flags, bits);
            return (Result) MethodHandles.publicLookup().findStatic(Client.class, "pick",
                MethodType.methodType(Result.class, Producer.class, Object.class, Context.class, long.class, int.class))
                .invokeExact(source, seed, context, flags, bits);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), variant = Integer.parseInt(args[1]);
        int fail = Integer.parseInt(args[2]), kind = Integer.parseInt(args[3]);
        metadata();
        Throwable sentinel = kind == 0 ? new IllegalStateException("owned subtype failure") : new LinkageError("owned subtype failure");
        for (int attempt = 0; attempt < 2; attempt++) {
            Object seed = variant == 1 ? null : variant == 7 ? new String("seed") : variant == 8 ? Integer.valueOf(37) : new Object();
            Object selectedValue = variant == 5 ? null : variant == 9 ? new String("value") : variant == 10 ? new int[] {19} : new Object();
            Object late = new Object();
            long flags = variant == 11 ? Long.MIN_VALUE + 37 : variant == 12 ? Long.MAX_VALUE - 19 : 0x12345678ABCDEF01L;
            int bits = variant == 13 ? Integer.MIN_VALUE : variant == 14 ? Integer.MAX_VALUE : 0x76543210;
            Input input = new Input(new Object());
            Receiver context = new Receiver(), other = new Receiver();
            Output result = new Output(), lateResult = new Output();
            Result selected = variant == 4 ? null : result;
            Trace.events = 0; Trace.mask = 0;
            Trace.seenProducer = Trace.seenSeed = Trace.seenValue = Trace.seenResult = null;
            Trace.seenFlags = 0; Trace.seenBits = 0;
            Trace.context = context; Trace.output = new Output(); Trace.lateValue = late;
            Trace.observer = event -> {
                if (event == 1) input.item = selectedValue;
                if (event == 5) input.item = late;
                if (event == 2) Trace.context = other;
                if (event == 3) Trace.output = selected;
                if (event == 4) Trace.output = lateResult;
                if (event == fail) raise(sentinel);
            };
            Result actual = null; Throwable caught = null;
            try { actual = call(route, variant == 2 ? null : input, seed, variant == 3 ? null : context, flags, bits); }
            catch (Throwable failure) { caught = failure; }
            boolean reachedFailure = fail != 0 && (variant != 2 || fail == 1) && (variant != 3 || fail == 1 || fail == 5 || fail == 2);
            if (reachedFailure) require(caught == sentinel);
            else if (variant == 2 || variant == 3) require(caught instanceof NullPointerException);
            else require(caught == null && actual == selected);
            long expected = 0; int mask = 0;
            for (int event : new int[] {1,5,2,6,3,4}) {
                if (event == 5 && variant == 2 || event == 6 && variant == 3) break;
                if (event == 1) mask |= 1;
                if (event == 2) mask |= 2;
                if (event == 3) mask |= 4;
                if (event == 4) mask |= 8;
                expected = expected * 10 + event;
                if (event == fail) break;
            }
            require(Trace.events == expected && Trace.mask == mask);
            require(Trace.seenProducer == (variant == 2 ? null : input) && Trace.seenSeed == seed);
            require(Trace.seenFlags == flags && Trace.seenBits == bits);
            if ((mask & 2) != 0) require(Trace.seenValue == selectedValue);
            if ((mask & 8) != 0) require(Trace.seenResult == selected);
            int reads = Long.toString(expected).indexOf('5') >= 0 ? 1 : 0;
            int calls = Long.toString(expected).indexOf('6') >= 0 ? 1 : 0;
            require(input.reads == reads && context.calls == calls && other.calls == 0);
            if (calls != 0) require(context.value == selectedValue && context.seed == seed && context.flags == flags && context.bits == bits);
            require(result.reads == 0 && lateResult.reads == 0);
            System.out.println("route=" + route + ",variant=" + variant + ",fail=" + fail + ",kind=" + kind + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
