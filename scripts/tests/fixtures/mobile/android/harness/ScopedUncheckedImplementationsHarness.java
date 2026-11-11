import fixture.*;
import java.lang.reflect.*;

public final class ScopedUncheckedImplementationsHarness {
    private static int values, identities, states, failures;
    private static final Object MARKER = new Object();
    private interface Call { Object run(Object input) throws Throwable; }
    private static final class Leaf extends Core<Object> {
        public Object read(Object input) throws First, Second {
            calls += 2;
            return super.read(input);
        }
        public <R> R unwrap(Class<R> kind) throws First {
            calls += 2;
            return super.unwrap(kind);
        }
    }
    private static Object invoke(Method method, Object receiver, Object input) throws Throwable {
        try { return method.invoke(receiver, input); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original fixed exception callback identity changed");
        identities++;
    }
    private static void reset(Core<?> receiver, First first, Second second) {
        receiver.calls = 0; receiver.seenInput = null; receiver.seenClass = null;
        receiver.failure = first; receiver.second = second;
    }
    private static void state(Core<?> receiver, int calls, Object input, Class<?> kind,
                              First first, Second second) {
        if (receiver.calls != calls) throw new AssertionError("original fixed exception effects changed");
        same(receiver.seenInput, input); same(receiver.seenClass, kind);
        same(receiver.failure, first); same(receiver.second, second);
        states++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Call[] paths(Core<?> receiver, String name, Class<?> parameter) throws Exception {
        Api api = receiver;
        Method concrete = Core.class.getDeclaredMethod(name, parameter);
        Method required = Api.class.getDeclaredMethod(name, parameter);
        if (name.equals("read"))
            return new Call[] {receiverInput -> ((Core) receiver).read(receiverInput), input -> api.read(input),
                               input -> invoke(concrete, receiver, input), input -> invoke(required, receiver, input)};
        return new Call[] {input -> receiver.unwrap((Class) input), input -> api.unwrap((Class) input),
                           input -> invoke(concrete, receiver, input), input -> invoke(required, receiver, input)};
    }
    public static void main(String[] args) throws Throwable {
        Method read = Core.class.getDeclaredMethod("read", Object.class);
        Method unwrap = Core.class.getDeclaredMethod("unwrap", Class.class);
        if (Core.class.getTypeParameters().length != 1 || Core.class.getInterfaces()[0] != Api.class
                || !Core.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Api<G>")
                || !read.getGenericParameterTypes()[0].getTypeName().equals("G")
                || !read.getGenericReturnType().getTypeName().equals("G")
                || Api.class.getDeclaredMethod("read", Object.class).getExceptionTypes().length != 0
                || Api.class.getDeclaredMethod("unwrap", Class.class).getExceptionTypes().length != 0
                || read.isBridge() || read.getTypeParameters().length != 0
                || read.getExceptionTypes().length != 2 || read.getExceptionTypes()[0] != First.class
                || read.getExceptionTypes()[1] != Second.class || unwrap.isBridge()
                || unwrap.getExceptionTypes().length != 1 || unwrap.getExceptionTypes()[0] != First.class
                || unwrap.getTypeParameters().length != 1
                || !unwrap.getGenericParameterTypes()[0].getTypeName().equals("java.lang.Class<R>")
                || !unwrap.getGenericReturnType().getTypeName().equals("R")
                || !Api.class.getDeclaredMethod("unwrap", Class.class).getGenericParameterTypes()[0]
                    .getTypeName().equals("java.lang.Class<T>"))
            throw new AssertionError("original fixed Exceptions or method-owned generic scope changed");
        for (Core<?> receiver : new Core<?>[] {new Core<Object>(), new Leaf()}) {
            int delta = receiver instanceof Leaf ? 3 : 1;
            Call[] reads = paths(receiver, "read", Object.class), absentReads = paths(null, "read", Object.class);
            for (int route = 0; route < reads.length; route++) {
                for (Object input : new Object[] {null, new Object(), new Object[] {MARKER}}) {
                    reset(receiver, null, null);
                    Object result = reads[route].run(input);
                    same(result, input); state(receiver, delta, input, null, null, null);
                    if (input instanceof Object[]) {
                        ((Object[]) result)[0] = receiver;
                        same(((Object[]) receiver.seenInput)[0], receiver);
                    }
                    values++;
                }
                Object input = new Object(); First first = new First(); Second second = new Second();
                reset(receiver, first, null);
                try { reads[route].run(input); throw new AssertionError("first fixed failure missing"); }
                catch (Throwable error) { same(error, first); failures++; }
                state(receiver, delta, input, null, first, null);
                reset(receiver, null, second);
                try { reads[route].run(input); throw new AssertionError("second fixed failure missing"); }
                catch (Throwable error) { same(error, second); failures++; }
                state(receiver, delta, input, null, null, second);
                reset(receiver, null, null);
                try { absentReads[route].run(input); throw new AssertionError("null read receiver missing"); }
                catch (Throwable error) { same(error.getClass(), NullPointerException.class); failures++; }
                state(receiver, 0, null, null, null, null);
            }
            Call[] unwraps = paths(receiver, "unwrap", Class.class), absentUnwraps = paths(null, "unwrap", Class.class);
            for (int route = 0; route < unwraps.length; route++) {
                for (int mode = 0; mode < 3; mode++) {
                    Class<?> kind = mode == 1 ? Object.class : String.class;
                    Object input = mode == 0 ? new String("owned input") : mode == 1 ? new Object[] {MARKER} : null;
                    receiver.value = input; reset(receiver, null, null);
                    Object result = unwraps[route].run(kind);
                    same(result, input); state(receiver, delta, input, kind, null, null);
                    if (input instanceof Object[]) {
                        ((Object[]) result)[0] = receiver;
                        same(((Object[]) receiver.seenInput)[0], receiver);
                    }
                    values++;
                }
                Object input = new String("owned failure input"); First first = new First();
                receiver.value = input; reset(receiver, first, null);
                try { unwraps[route].run(String.class); throw new AssertionError("fixed unwrap failure missing"); }
                catch (Throwable error) { same(error, first); failures++; }
                state(receiver, delta, input, String.class, first, null);
                Object wrong = new Object(); receiver.value = wrong; reset(receiver, null, null);
                try { unwraps[route].run(String.class); throw new AssertionError("late result cast missing"); }
                catch (Throwable error) { same(error.getClass(), ClassCastException.class); failures++; }
                state(receiver, delta, wrong, String.class, null, null);
                receiver.value = input; reset(receiver, null, null);
                try { unwraps[route].run(null); throw new AssertionError("late null Class failure missing"); }
                catch (Throwable error) { same(error.getClass(), NullPointerException.class); failures++; }
                state(receiver, delta, input, null, null, null);
                reset(receiver, null, null);
                try { absentUnwraps[route].run(String.class); throw new AssertionError("null unwrap receiver missing"); }
                catch (Throwable error) { same(error.getClass(), NullPointerException.class); failures++; }
                state(receiver, 0, null, null, null, null);
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("unchecked-exception-order=retained,complete-class-and-method-formals=retained,original-runtime-entries=retained");
    }
}
