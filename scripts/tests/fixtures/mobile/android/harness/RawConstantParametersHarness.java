import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.WildcardType;

public final class RawConstantParametersHarness {
    private static int values, identities, states, failures, touches;
    private static final Method CORE, API;
    private static final MethodHandle CORE_HANDLE, API_HANDLE;
    static {
        try {
            CORE = Core.class.getMethod("run", Box.class, Context.class);
            API = Api.class.getMethod("run", Box.class, Context.class);
            MethodType type = MethodType.methodType(Object.class, Box.class, Context.class);
            CORE_HANDLE = MethodHandles.lookup().findVirtual(Core.class, "run", type);
            API_HANDLE = MethodHandles.lookup().findVirtual(Api.class, "run", type);
        } catch (Exception error) { throw new AssertionError(error); }
    }
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed");
        identities++;
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("state changed: " + expected + ":" + actual);
        states++;
    }
    private static Object caller(Api api, Box box, Context context) {
        return api.run(box, context);
    }
    private static Object call(int route, Core core, Box box, Context context) throws Throwable {
        try {
            switch (route) {
                case 0: return core.run(box, context);
                case 1: return ((Api) core).run(box, context);
                case 2: return ((Api<Object>) core).run(box, context);
                case 3: return CORE.invoke(core, box, context);
                case 4: return API.invoke(core, box, context);
                case 5: return (Object) CORE_HANDLE.invokeExact(core, box, context);
                case 6: return (Object) API_HANDLE.invokeExact((Api) core, box, context);
                case 7: return caller(core, box, context);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void metadata(boolean original) {
        if (Core.class.getGenericInterfaces()[0] != Api.class || Core.class.getTypeParameters().length != 0
                || CORE.getTypeParameters().length != 0 || CORE.getReturnType() != Object.class
                || CORE.isBridge() || CORE.isSynthetic())
            throw new AssertionError("raw interface or original entry changed");
        Type[] parameters = CORE.getGenericParameterTypes();
        if (original) {
            WildcardType first = (WildcardType) ((ParameterizedType) parameters[0]).getActualTypeArguments()[0];
            WildcardType second = (WildcardType) ((ParameterizedType) parameters[1]).getActualTypeArguments()[0];
            if (((ParameterizedType) parameters[0]).getRawType() != Box.class
                    || first.getLowerBounds().length != 0 || first.getUpperBounds()[0] != Object.class
                    || ((ParameterizedType) parameters[1]).getRawType() != Context.class
                    || second.getLowerBounds()[0] != Token.class || second.getUpperBounds()[0] != Object.class)
                throw new AssertionError("original closed parameter metadata changed");
        } else if (parameters[0] != Box.class || parameters[1] != Context.class) {
            throw new AssertionError("source parameter erasure not reflected");
        }
    }
    public static void main(String[] args) throws Throwable {
        metadata(args[0].equals("original"));
        Core core = new Core();
        RuntimeException failure = new RuntimeException("owned-initialization");
        State.failure = failure;
        State.fail = args[1].equals("failure");
        state(0, State.initializations);
        for (int route = 0; route < 8; route++) {
            try { call(route, null, null, null); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
            state(0, State.initializations);
        }
        if (State.fail) {
            int first = Integer.parseInt(args[2]);
            try { call(first, core, null, null); throw new AssertionError("initialization failure missing"); }
            catch (ExceptionInInitializerError error) { same(failure, error.getCause()); failures++; }
            state(1, State.initializations);
            for (int route = 0; route < 8; route++) {
                try { call(route, core, null, null); throw new AssertionError("erroneous class accepted"); }
                catch (NoClassDefFoundError expected) { failures++; }
                state(1, State.initializations);
            }
        } else {
            Box[] boxes = {null, new Box<String>() {
                public String get() { touches++; throw new AssertionError("input was read"); }
            }, new Box<Object>() {
                public Object get() { touches++; throw new AssertionError("input was read"); }
            }};
            Context[] contexts = {null, new Context<Token>() {
                public void accept(Token value) { touches++; throw new AssertionError("context was called"); }
            }, new Context<Object>() {
                public void accept(Object value) { touches++; throw new AssertionError("context was called"); }
            }};
            for (int route = 0; route < 8; route++) {
                for (Box box : boxes) for (Context context : contexts) {
                    core.marker = box;
                    Object result = call(route, core, box, context);
                    same(Token.VALUE, result); same(box, core.marker);
                    Token token = (Token) result;
                    int before = token.value;
                    token.value += 3;
                    same(token, call(route, core, box, context)); same(box, core.marker);
                    state(before + 3, token.value); state(1, State.initializations); state(0, touches);
                    values += 2;
                }
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("raw-interface-and-field-read=retained,closed-parameter-source-erasure=reported");
    }
}
