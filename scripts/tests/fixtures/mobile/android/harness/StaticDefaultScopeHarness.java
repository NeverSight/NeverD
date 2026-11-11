import fixture.Action;
import fixture.Holder;
import fixture.Scopes;
import fixture.State;
import fixture.Token;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;

public final class StaticDefaultScopeHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("Static default scope behavior changed");
    }
    private static void reset() {
        State.constructors = State.reads = State.events = 0;
        State.lastToken = null;
        State.lastTag = null;
        State.lastAction = null;
        State.constructorFailure = State.readFailure = null;
    }
    private static Action<Object> supplied(final Token<Object> expected, final Object tag) {
        return new Action<Object>() {
            public Object apply(Holder holder) {
                require(holder.token == expected && holder.tag == tag);
                return holder.token.read();
            }
        };
    }
    public static void main(String[] args) throws Exception {
        int probes = 0;
        for (Object value : new Object[] {new Object(), "owned", Integer.valueOf(51), null}) {
            Token<Object> token = new Token<Object>(value);
            Object tag = new Object();
            reset();
            require(Scopes.make(token, tag, supplied(token, tag)) == value);
            require(State.constructors == 1 && State.reads == 1 && State.events == 12);
            ++probes;
            for (int mask : new int[] {0, 1, 2, 3, 4, 5}) {
                reset();
                Object selectedTag = (mask & 2) == 0 ? tag : null;
                require(Scopes.make$default(token, tag, supplied(token, selectedTag), mask, new Object()) == value);
                require(State.lastToken == token && State.lastTag == selectedTag);
                require(State.constructors == 1 && State.reads == 1 && State.events == 12);
                ++probes;
            }
            Object next = new Object();
            token.value = next;
            reset();
            require(Scopes.make$default(token, tag, null, 1, null) == next && State.events == 12);
            ++probes;
        }
        Class<?> callback = State.lastAction.getClass();
        Token<Object> token = new Token<Object>(new Object());
        for (int mask : new int[] {0, 1}) {
            RuntimeException sentinel = new RuntimeException("owned constructor failure");
            reset();
            State.constructorFailure = sentinel;
            try { Scopes.make$default(token, null, null, mask, null); throw new AssertionError("missing constructor failure"); }
            catch (RuntimeException actual) { require(actual == sentinel && State.events == 1 && State.reads == 0); }
            reset();
            State.readFailure = sentinel;
            try { Scopes.make$default(token, null, supplied(token, null), mask, null); throw new AssertionError("missing read failure"); }
            catch (RuntimeException actual) { require(actual == sentinel && State.events == 12 && State.reads == 1); }
        }
        reset();
        try { Scopes.make$default(token, null, null, 0, null); throw new AssertionError("missing null action failure"); }
        catch (NullPointerException expected) { require(State.events == 1 && State.reads == 0); }
        reset();
        try { Scopes.make$default(null, null, null, 1, null); throw new AssertionError("missing null token failure"); }
        catch (NullPointerException expected) { require(State.events == 1 && State.reads == 0); }
        Method generic = Scopes.class.getDeclaredMethod("make", Token.class, Object.class, Action.class);
        require(generic.getTypeParameters().length == 1 && generic.getTypeParameters()[0].getName().equals("E"));
        Type original = ((ParameterizedType) generic.getGenericParameterTypes()[0]).getActualTypeArguments()[0];
        require(original instanceof TypeVariable && ((TypeVariable<?>) original).getGenericDeclaration() == generic);
        Method wrapper = Scopes.class.getDeclaredMethod("make$default", Token.class, Object.class, Action.class, int.class, Object.class);
        require(wrapper.getTypeParameters().length == 0);
        require(callback.getEnclosingMethod().equals(wrapper));
        Type classArgument = ((ParameterizedType) callback.getGenericInterfaces()[0]).getActualTypeArguments()[0];
        callback.getDeclaredMethod("apply", Holder.class);
        Type fieldArgument = ((ParameterizedType) callback.getDeclaredField("snapshot").getGenericType()).getActualTypeArguments()[0];
        if (args[0].equals("original")) require(classArgument == null && fieldArgument == null);
        else require(classArgument == Object.class && fieldArgument == Object.class);
        System.out.println("identity-probes=" + probes + ";ordered-failures=6;metadata-differences=2");
    }
}
