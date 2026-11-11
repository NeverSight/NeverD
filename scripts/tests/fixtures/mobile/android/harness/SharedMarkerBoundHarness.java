import fixture.FirstFlag;
import fixture.Flag;
import fixture.Owner;
import fixture.Reader;
import fixture.SecondFlag;
import fixture.Token;
import fixture.Value;
import java.lang.reflect.MalformedParameterizedTypeException;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.WildcardType;

public final class SharedMarkerBoundHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("Shared marker bound behavior changed");
    }
    private static Type argument(Type type) { return ((ParameterizedType) type).getActualTypeArguments()[0]; }
    private static void marker(Type type) {
        require(type instanceof ParameterizedType && ((ParameterizedType) type).getRawType() == Flag.class);
        Type value = argument(type);
        require(value instanceof WildcardType && ((WildcardType) value).getLowerBounds().length == 0
                && ((WildcardType) value).getUpperBounds().length == 1 && ((WildcardType) value).getUpperBounds()[0] == Value.class);
    }
    private static Flag<Value> flag(boolean first, Object payload) {
        Value value = new Value();
        value.payload = payload;
        if (first) {
            FirstFlag flag = new FirstFlag();
            flag.value = value;
            return flag;
        }
        SecondFlag flag = new SecondFlag();
        flag.value = value;
        return flag;
    }
    private static void variable(Type type, Method owner) {
        require(type instanceof TypeVariable && ((TypeVariable<?>) type).getName().equals("T")
                && ((TypeVariable<?>) type).getGenericDeclaration().equals(owner));
        marker(((TypeVariable<?>) type).getBounds()[0]);
    }
    public static void main(String[] args) throws Exception {
        int probes = 0, failures = 0;
        Class<?> worker = null;
        for (boolean first : new boolean[] {true, false}) {
            for (Object payload : new Object[] {new Object(), "owned", Integer.valueOf(29), null}) {
                Flag<Value> value = flag(first, payload);
                Token<Flag<Value>> token = new Token<Flag<Value>>(value);
                Object original = Owner.make(token);
                worker = original.getClass();
                Object copy = worker.getDeclaredMethod("copy").invoke(original);
                Object another = Owner.another(token);
                require(copy != original && another != copy && another != original && copy.getClass() == worker && another.getClass() == worker);
                require(worker.getField("token").get(original) == token && worker.getField("token").get(copy) == token);
                require(((Reader) original).read() == value && ((Reader) copy).read() == value && token.reads == 2);
                probes += 2;
                Flag<Value> next = flag(!first, new Object());
                token.value = next;
                require(((Reader) original).read() == next && ((Reader) copy).read() == next && token.reads == 4);
                probes += 2;
                RuntimeException sentinel = new RuntimeException("owned marker failure");
                token.failure = sentinel;
                try { ((Reader) copy).read(); throw new AssertionError("missing marker token failure"); }
                catch (RuntimeException actual) { require(actual == sentinel && token.reads == 5); ++failures; }
                require(argument(next.getClass().getGenericInterfaces()[0]) == Value.class);
            }
        }
        Token<Flag<Value>> token = new Token<Flag<Value>>(null);
        Object original = Owner.make(token);
        Object copy = worker.getDeclaredMethod("copy").invoke(original);
        require(((Reader) original).read() == null && ((Reader) copy).read() == null && token.reads == 2);
        probes += 2;
        Reader empty = (Reader) Owner.<Flag<Value>>make(null);
        try { empty.read(); throw new AssertionError("missing null marker token failure"); }
        catch (NullPointerException expected) { ++failures; }
        Object clone = empty.getClass().getDeclaredMethod("copy").invoke(empty);
        require(clone != empty && clone.getClass() == worker && worker.getField("token").get(clone) == null);
        try { ((Reader) clone).read(); throw new AssertionError("missing copied null marker token failure"); }
        catch (NullPointerException expected) { ++failures; }
        Method make = Owner.class.getDeclaredMethod("make", Token.class);
        require(make.getTypeParameters().length == 1 && Owner.class.getTypeParameters().length == 0 && worker.getTypeParameters().length == 0);
        marker(make.getTypeParameters()[0].getBounds()[0]);
        Type field = argument(worker.getField("token").getGenericType());
        Type constructor = argument(worker.getDeclaredConstructor(Token.class).getGenericParameterTypes()[0]);
        Type result = worker.getDeclaredMethod("read").getGenericReturnType();
        if (args[0].equals("original")) {
            require(worker.getEnclosingMethod().equals(make));
            variable(field, make); variable(constructor, make); variable(result, make);
            try {
                worker.getDeclaredMethod("copy").getGenericReturnType();
                throw new AssertionError("missing marker phantom self reflection failure");
            } catch (MalformedParameterizedTypeException expected) {}
        } else {
            require(worker.getEnclosingMethod() == null && worker.getDeclaringClass() == Owner.class);
            marker(field); marker(constructor); marker(result);
            require(worker.getDeclaredMethod("copy").getGenericReturnType() == worker);
        }
        System.out.println("returned-identities=" + probes + ";ordered-failures=" + failures + ";metadata-differences=4");
    }
}
