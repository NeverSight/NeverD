import fixture.FirstBound;
import fixture.Owner;
import fixture.Reader;
import fixture.SecondBound;
import fixture.Token;
import fixture.UpperBound;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.MalformedParameterizedTypeException;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;

public final class SharedUpperBoundHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("Shared upper bound behavior changed");
    }
    private static Type argument(Type type) { return ((ParameterizedType) type).getActualTypeArguments()[0]; }
    private static void variable(Type type) {
        require(type instanceof TypeVariable && ((TypeVariable<?>) type).getName().equals("T")
                && ((TypeVariable<?>) type).getGenericDeclaration() == Owner.class);
    }
    public static void main(String[] args) throws Exception {
        Owner<UpperBound> owner = new Owner<UpperBound>();
        int probes = 0, failures = 0;
        Class<?> worker = null;
        for (boolean first : new boolean[] {true, false}) {
            for (Object payload : new Object[] {new Object(), "owned", Integer.valueOf(73), null}) {
                UpperBound value = first ? new FirstBound() : new SecondBound();
                value.value = payload;
                Token<UpperBound> token = new Token<UpperBound>(value);
                Object original = owner.make(token);
                worker = original.getClass();
                Object copy = worker.getDeclaredMethod("copy").invoke(original);
                Object another = owner.another(token);
                require(another != original && another != copy && another.getClass() == worker);
                require(copy != original && copy.getClass() == worker);
                require(worker.getField("token").get(original) == token && worker.getField("token").get(copy) == token);
                require(((Reader<?>) original).read() == value && ((Reader<?>) copy).read() == value && value.reads == 2);
                probes += 2;
                UpperBound next = first ? new SecondBound() : new FirstBound();
                next.value = new Object();
                token.value = next;
                require(((Reader<?>) original).read() == next && ((Reader<?>) copy).read() == next && next.reads == 2);
                probes += 2;
                RuntimeException sentinel = new RuntimeException("owned token failure");
                token.failure = sentinel;
                try { ((Reader<?>) original).read(); throw new AssertionError("missing token failure"); }
                catch (RuntimeException actual) { require(actual == sentinel && next.reads == 2); ++failures; }
                token.failure = null;
                next.failure = sentinel;
                try { ((Reader<?>) copy).read(); throw new AssertionError("missing nominal member failure"); }
                catch (RuntimeException actual) { require(actual == sentinel && next.reads == 3); ++failures; }
                next.failure = null;
                token.value = null;
                try { ((Reader<?>) original).read(); throw new AssertionError("missing null bound failure"); }
                catch (NullPointerException expected) { require(next.reads == 3); ++failures; }
            }
        }
        Reader<?> empty = (Reader<?>) owner.make(null);
        try { empty.read(); throw new AssertionError("missing null token failure"); }
        catch (NullPointerException expected) { ++failures; }
        Object clone = empty.getClass().getDeclaredMethod("copy").invoke(empty);
        require(clone != empty && clone.getClass() == empty.getClass() && empty.getClass().getField("token").get(clone) == null);
        try { ((Reader<?>) clone).read(); throw new AssertionError("missing copied null token failure"); }
        catch (NullPointerException expected) { ++failures; }
        require(worker.getTypeParameters().length == 0);
        if (args[0].equals("original")) require(worker.getEnclosingMethod().getName().equals("make"));
        else require(worker.getEnclosingMethod() == null && worker.getDeclaringClass() == Owner.class);
        require(Owner.class.getTypeParameters()[0].getBounds()[0] == UpperBound.class);
        Type contract = argument(worker.getGenericInterfaces()[0]);
        Type field = argument(worker.getField("token").getGenericType());
        Type constructor = argument(worker.getDeclaredConstructor(Token.class).getGenericParameterTypes()[0]);
        Type result = worker.getDeclaredMethod("read").getGenericReturnType();
        if (args[0].equals("original")) {
            variable(contract); variable(field); variable(constructor); variable(result);
            try {
                worker.getDeclaredMethod("copy").getGenericReturnType();
                throw new AssertionError("missing phantom self argument reflection failure");
            } catch (MalformedParameterizedTypeException expected) {}
        } else {
            require(contract == UpperBound.class && field == UpperBound.class && constructor == UpperBound.class
                    && result == UpperBound.class && worker.getDeclaredMethod("copy").getGenericReturnType() == worker);
        }
        System.out.println("returned-identities=" + probes + ";ordered-failures=" + failures + ";metadata-differences=5");
    }
}
