import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class SeededCarrierConstructorHarness {
    static int route, mode, attempt;
    static boolean future;
    static Object originalSeed;
    static Token<?> expectedToken;
    static Value value;
    static Throwable selected;
    static float expectedFloat;
    static int[] counts;
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned seeded carrier constructor mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    static void receiver(Object seed) {
        require(Trace.receiver instanceof Product && Trace.seed == seed && Trace.token == expectedToken);
        if (route == 6) {
            require(seed instanceof Host && ((Host<?>) seed).value == value && ((Host<?>) seed).product == null);
        } else require(seed == originalSeed);
    }
    static void reading(Object seed) {
        if (future) Trace.hit(9);
        Trace.hit(4);
        require(++counts[1] == 1 && counts[2] == 0 && counts[3] == 0);
        receiver(seed);
        if (mode >= 4 && mode <= 6) SeededCarrierConstructorHarness.<RuntimeException>raise(selected);
    }
    static float readValue(Value seed) {
        require(seed.tag == 17 + attempt);
        return expectedFloat;
    }
    static void writing(Object seed, float observed) {
        Trace.hit(6);
        require(++counts[2] == 1 && counts[1] == 1 && counts[3] == 0);
        receiver(seed);
        require(Float.floatToRawIntBits(observed) == Float.floatToRawIntBits(expectedFloat));
        require(Float.floatToRawIntBits(Trace.observed) == Float.floatToRawIntBits(expectedFloat));
        if (mode == 7 || mode == 8) SeededCarrierConstructorHarness.<RuntimeException>raise(selected);
        value.tag = 91;
    }
    static final class Loose extends Token<Object> {
        public float read(Object seed) {
            reading(seed);
            return readValue(route == 6 ? ((Host<?>) seed).value : (Value) seed);
        }
        public void write(Object seed, float observed) { writing(seed, observed); }
    }
    static final class Typed extends Token<Value> {
        public float read(Value seed) { reading(seed); return readValue(seed); }
        public void write(Value seed, float observed) { writing(seed, observed); }
    }
    static final class SelfTyped extends Token<Host<?>> {
        public float read(Host<?> seed) { reading(seed); return readValue(seed.value); }
        public void write(Host<?> seed, float observed) { writing(seed, observed); }
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    static Token<Object> token() {
        return (Token) (future ? route == 6 ? new SelfTyped() : new Typed() : new Loose());
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void main(String[] args) throws Throwable {
        route = Integer.parseInt(args[0]); future = Boolean.parseBoolean(args[1]); mode = Integer.parseInt(args[2]);
        for (int arity : new int[]{2, 3}) {
            Constructor<?> declaration = arity == 2 ? Product.class.getConstructor(Object.class, Token.class)
                    : Product.class.getConstructor(Object.class, Token.class, float.class);
            TypeVariable<?>[] variables = declaration.getTypeParameters();
            require(variables.length == 1 && variables[0].getGenericDeclaration().equals(declaration));
            require(variables[0].getBounds().length == 1 && variables[0].getBounds()[0] == Object.class);
            Type[] inputs = declaration.getGenericParameterTypes();
            require(inputs[0].equals(variables[0]) && ((ParameterizedType) inputs[1]).getRawType() == Token.class);
            require(((ParameterizedType) inputs[1]).getActualTypeArguments()[0].equals(variables[0]));
            require(declaration.getExceptionTypes().length == 0);
            if (arity == 3) require(inputs[2] == float.class);
        }
        require(Product.class.getDeclaredConstructors().length == 3 && Product.class.getTypeParameters().length == 0);
        require(((ParameterizedType) Product.class.getGenericSuperclass()).getActualTypeArguments()[0] == Product.class);
        TypeVariable<?> parent = Parent.class.getTypeParameters()[0];
        ParameterizedType bound = (ParameterizedType) parent.getBounds()[0];
        require(bound.getRawType() == Parent.class && bound.getActualTypeArguments()[0].equals(parent));
        Constructor<?> base = Parent.class.getDeclaredConstructor(Object.class, Token.class);
        require(base.getModifiers() == 0 && base.getTypeParameters()[0].getGenericDeclaration().equals(base));
        require(!base.getTypeParameters()[0].equals(parent) && base.getExceptionTypes().length == 0);
        Method method = Core.class.getMethod("make", Object.class, Token.class);
        MethodHandle handle = MethodHandles.lookup().findStatic(Core.class, "make",
                MethodType.methodType(Product.class, Object.class, Token.class));
        for (attempt = 0; attempt < 2; ++attempt) {
            value = mode == 1 ? null : new Value();
            if (value != null) value.tag = 17 + attempt;
            originalSeed = mode == 2 && route != 6 ? new Object() : value;
            final Token<Object> first = mode == 3 ? null : token();
            final Token<Object> replacement = mode == 3 ? null : token();
            Host.current = (Token) first;
            expectedToken = route == 6 && attempt == 0 ? replacement : first;
            selected = mode == 5 || mode == 7 || mode == 10 || mode == 14 ? new java.io.IOException()
                    : mode == 6 || mode == 8 || mode == 11 || mode == 15 ? new AssertionError() : new IllegalStateException();
            expectedFloat = mode == 12 ? Float.intBitsToFloat(0x7fc12345)
                    : mode == 13 ? -0.0f : Float.POSITIVE_INFINITY;
            final float weight = attempt == 0 ? Float.intBitsToFloat(0x7fc12345) : -0.0f;
            counts = new int[4];
            Trace.events = 0; Trace.receiver = null; Trace.seed = null; Trace.token = null;
            Trace.observed = 0; Trace.weight = 0; Trace.kind = 0;
            Trace.initAction = new Runnable() { public void run() {
                require(++counts[0] == 1 && Trace.events == 12 && Trace.receiver == null);
                Host.current = (Token) replacement;
                if (mode >= 14) SeededCarrierConstructorHarness.<RuntimeException>raise(selected);
            }};
            Trace.action = new Runnable() { public void run() {
                require(++counts[3] == 1 && counts[1] == 1 && counts[2] == 1 && value.tag == 91);
                receiver(Trace.seed);
                if (mode >= 9 && mode <= 11) SeededCarrierConstructorHarness.<RuntimeException>raise(selected);
            }};
            Product result = null; Host<?> host = null; Throwable failure = null;
            try {
                if (route == 0) result = Core.make(originalSeed, first);
                else if (route == 1) {
                    try { result = (Product) method.invoke(null, originalSeed, first); }
                    catch (InvocationTargetException caught) { throw caught.getCause(); }
                } else if (route == 2) result = (Product) handle.invokeExact(originalSeed, first);
                else if (route == 3) result = Core.weighted(originalSeed, first, weight);
                else if (route == 4) result = Core.nested(originalSeed, first);
                else if (route == 5) result = SeededCarrierConstructorCaller.call(originalSeed, first);
                else { host = new Host<Value>(value); result = host.product; }
            } catch (Throwable caught) { failure = caught; }
            boolean entered = mode < 14;
            boolean badSeed = mode == 2 && route != 6;
            boolean read = entered && mode != 3 && !(badSeed && future);
            boolean readSucceeded = read && mode != 1 && !badSeed && !(mode >= 4 && mode <= 6);
            boolean writeSucceeded = readSucceeded && mode != 7 && mode != 8;
            boolean completed = writeSucceeded && !(mode >= 9 && mode <= 11);
            String sequence = "1" + (attempt == 0 ? "2" : "");
            if (entered) sequence += "3";
            if (read) sequence += (future ? "9" : "") + "4";
            if (readSucceeded) sequence += "56";
            if (writeSucceeded) sequence += "7";
            if (completed) sequence += "98";
            require(Trace.events == Long.parseLong(sequence));
            require(counts[0] == (attempt == 0 ? 1 : 0));
            require(counts[1] == (read ? 1 : 0) && counts[2] == (readSucceeded ? 1 : 0));
            require(counts[3] == (writeSucceeded ? 1 : 0));
            require((Trace.receiver != null) == entered && Trace.token == (entered ? expectedToken : null));
            if (entered) {
                if (route == 6) require(Trace.seed instanceof Host && ((Host<?>) Trace.seed).value == value);
                else require(Trace.seed == originalSeed);
            } else require(Trace.seed == null);
            require(Float.floatToRawIntBits(Trace.observed) == (readSucceeded ? Float.floatToRawIntBits(expectedFloat) : 0));
            require(Float.floatToRawIntBits(Trace.weight) == (completed && route == 3 ? Float.floatToRawIntBits(weight) : 0));
            require(Trace.kind == (completed ? route == 3 ? 3 : 2 : 0));
            require(result == (completed ? Trace.receiver : null));
            if (mode >= 14 && attempt == 1) require(failure instanceof NoClassDefFoundError);
            else if (mode == 14) require(failure instanceof ExceptionInInitializerError && failure.getCause() == selected);
            else if (mode == 15) require(failure == selected);
            else if (mode == 1 || mode == 3) require(failure instanceof NullPointerException);
            else if (badSeed) require(failure instanceof ClassCastException);
            else if (mode >= 4 && mode <= 11) require(failure == selected);
            else require(failure == null);
            if (host != null) require(host == Trace.seed && host.product == Trace.receiver);
        }
        System.out.println("attempts=2,seeded-carrier-constructor=matched");
    }
}
