import fixture.Pair;
import fixture.Owner;
import fixture.Reader;
import fixture.Root;
import fixture.RootBase;
import fixture.StateView;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.TypeVariable;

public final class PlainClassChainDefaultHarness {
    private static final class Child extends Reader<Object> {
        RuntimeException identityFailure;

        @Override
        public <N> N identity(N value) {
            rootCalls += 5;
            if (identityFailure != null) throw identityFailure;
            return super.identity(value);
        }
    }

    public static void main(String[] args) throws Throwable {
        NestedDefaultAncestorHarness.main(args);
        if (Owner.State.class.getGenericSuperclass() != Root.class
                || Root.class.getGenericSuperclass() != RootBase.class
                || RootBase.class.getGenericSuperclass() != Object.class
                || RootBase.class.getGenericInterfaces()[0] != StateView.class
                || Root.class.getTypeParameters().length != 0
                || RootBase.class.getTypeParameters().length != 0)
            throw new AssertionError("plain class or interface chain changed");
        Method identity = Reader.class.getMethod("identity", Object.class);
        TypeVariable<Method> variable = identity.getTypeParameters()[0];
        if (identity.getDeclaringClass() != RootBase.class
                || !variable.getGenericDeclaration().equals(identity)
                || variable.getBounds()[0] != Object.class
                || identity.getGenericParameterTypes()[0] != variable
                || identity.getGenericReturnType() != variable)
            throw new AssertionError("ancestor method-owned scope or ownership changed");
        Method pair = Reader.class.getMethod("pair", Pair.class, Object.class);
        TypeVariable<Method> pairVariable = pair.getTypeParameters()[0];
        ParameterizedType box = (ParameterizedType) pair.getGenericParameterTypes()[0];
        if (pair.getDeclaringClass() != RootBase.class || box.getRawType() != Pair.class
                || box.getActualTypeArguments()[0] != pairVariable
                || pair.getGenericParameterTypes()[1] != pairVariable
                || pair.getGenericReturnType() != pairVariable)
            throw new AssertionError("complete ancestor parameter and result scope changed");
        Reader<Object> reader = new Reader<>();
        RootBase alias = reader;
        if (((StateView) reader).rootState() != 3)
            throw new AssertionError("distant ancestor initialization changed");
        Object value = new Object();
        Pair<Object> boxValue = new Pair<>(value);
        if (reader.identity(value) != value || alias.identity(null) != null
                || identity.invoke(reader, value) != value || reader.pair(boxValue, value) != value
                || reader.rootState() != 7 || boxValue.value != value)
            throw new AssertionError("ancestor identities or mutable state changed");
        alias.rootCalls = 11;
        if (((StateView) reader).rootState() != 11)
            throw new AssertionError("ancestor interface lost mutable alias");
        Child child = new Child();
        if (child.identity(value) != value || child.rootState() != 9
                || identity.invoke(child, value) != value || child.rootState() != 15)
            throw new AssertionError("ancestor method lost virtual override");
        RuntimeException failure = new IllegalStateException("owned class chain failure");
        child.identityFailure = failure;
        try {
            child.identity(value);
            throw new AssertionError("original virtual failure disappeared");
        } catch (RuntimeException problem) {
            if (problem != failure || child.rootState() != 20)
                throw new AssertionError("virtual failure identity or effect order changed");
        }
        try {
            identity.invoke(child, value);
            throw new AssertionError("original reflected failure disappeared");
        } catch (InvocationTargetException problem) {
            if (problem.getCause() != failure || child.rootState() != 25)
                throw new AssertionError("reflected failure identity or effect order changed");
        }
        System.out.println("plain-class-chain=complete,original-generic-members-and-virtual-effects=matched");
    }
}
