import fixture.*;
import java.lang.reflect.*;

public final class RecursiveUncheckedCallbacksHarness {
    private static int values, identities, states, failures;
    private interface Call { Object run(byte[] input) throws Throwable; }
    private static final class Leaf extends Core {
        public byte[] update(byte[] input) throws LeafFailure, FatalFailure {
            calls += 2;
            return super.update(input);
        }
    }
    private static Object invoke(Method method, Object receiver, byte[] input) throws Throwable {
        try { return method.invoke(receiver, (Object) input); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original recursive callback identity changed");
        identities++;
    }
    private static void reset(Core receiver, LeafFailure failure, FatalFailure fatal) {
        receiver.calls = 0; receiver.forwardingCalls = 0; receiver.seen = null;
        receiver.failure = failure; receiver.fatal = fatal;
    }
    private static void state(Core receiver, int calls, int forwards, byte[] input,
                              LeafFailure failure, FatalFailure fatal) {
        if (receiver.calls != calls || receiver.forwardingCalls != forwards)
            throw new AssertionError("original recursive callback dispatch or effects changed");
        same(receiver.seen, input); same(receiver.failure, failure); same(receiver.fatal, fatal);
        states++;
    }
    private static Call[] paths(Core receiver) throws Exception {
        Updates view = receiver; Parent<Core> parent = receiver;
        Method concrete = Core.class.getDeclaredMethod("update", byte[].class);
        Method required = Updates.class.getDeclaredMethod("update", byte[].class);
        Method adapted = Updates.class.getDeclaredMethod("adapt", byte[].class);
        Method forwarding = Parent.class.getDeclaredMethod("forward", byte[].class);
        return new Call[] {input -> receiver.update(input), input -> view.update(input),
                           input -> parent.update(input), input -> view.adapt(input),
                           input -> parent.forward(input), input -> invoke(concrete, receiver, input),
                           input -> invoke(required, receiver, input), input -> invoke(adapted, receiver, input),
                           input -> invoke(forwarding, receiver, input)};
    }
    public static void main(String[] args) throws Throwable {
        Method target = Core.class.getDeclaredMethod("update", byte[].class);
        if (!Base.class.getTypeParameters()[0].getBounds()[0].getTypeName().equals("fixture.Base<B>")
                || !Parent.class.getTypeParameters()[0].getBounds()[0].getTypeName().equals("fixture.Parent<P>")
                || !Parent.class.getGenericSuperclass().getTypeName().equals("fixture.Base<P>")
                || !Core.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<fixture.Core>")
                || Parent.class.getInterfaces()[0] != Updates.class
                || Parent.class.getMethod("update", byte[].class).getDeclaringClass() != Updates.class
                || !Updates.class.getDeclaredMethod("adapt", byte[].class).isDefault()
                || Updates.class.getDeclaredMethod("update", byte[].class).getExceptionTypes().length != 0
                || target.isBridge() || target.getTypeParameters().length != 0
                || target.getExceptionTypes().length != 2
                || target.getExceptionTypes()[0] != LeafFailure.class
                || target.getExceptionTypes()[1] != FatalFailure.class)
            throw new AssertionError("original recursive bounds, ownership or unchecked declarations changed");
        for (Method method : Parent.class.getDeclaredMethods())
            if (method.getName().equals("update"))
                throw new AssertionError("parent acquired a callback declaration");
        for (Core receiver : new Core[] {new Core(), new Leaf()}) {
            int delta = receiver instanceof Leaf ? 3 : 1;
            Call[] paths = paths(receiver);
            Call[] absent = paths(null);
            for (int route = 0; route < paths.length; route++) {
                int forwards = route == 4 || route == 8 ? 1 : 0;
                for (byte[] input : new byte[][] {new byte[] {17, -3}, null}) {
                    reset(receiver, null, null);
                    Object result = paths[route].run(input);
                    same(result, input);
                    state(receiver, delta, forwards, input, null, null);
                    if (input != null) {
                        ((byte[]) result)[0] = 29;
                        if (receiver.seen[0] != 29) throw new AssertionError("mutable byte array alias changed");
                    }
                    values++;
                }
                byte[] input = new byte[] {41};
                LeafFailure problem = new LeafFailure();
                reset(receiver, problem, null);
                try { paths[route].run(input); throw new AssertionError("unchecked callback failure missing"); }
                catch (Throwable error) { same(error, problem); failures++; }
                state(receiver, delta, forwards, input, problem, null);
                FatalFailure fatal = new FatalFailure();
                reset(receiver, null, fatal);
                try { paths[route].run(input); throw new AssertionError("unchecked callback error missing"); }
                catch (Throwable error) { same(error, fatal); failures++; }
                state(receiver, delta, forwards, input, null, fatal);
                reset(receiver, null, null);
                try { absent[route].run(null); throw new AssertionError("null callback receiver failure missing"); }
                catch (Throwable error) { same(error.getClass(), NullPointerException.class); failures++; }
                state(receiver, 0, 0, null, null, null);
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("recursive-parent-bounds=retained,original-unchecked-exceptions=retained,parent-update-declarations=absent");
    }
}
