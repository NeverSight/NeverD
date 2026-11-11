import fixture.Owner;
import fixture.Reader;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;

public final class GenericAnonymousMemberHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("Anonymous member scope behavior changed");
    }
    private static Type argument(Type type) { return ((ParameterizedType) type).getActualTypeArguments()[0]; }
    public static void main(String[] args) throws Exception {
        int probes = 0, failures = 0;
        Class<?> member = null;
        for (Object value : new Object[] {new Object(), "owned", Integer.valueOf(61), null}) {
            Owner<Object> owner = new Owner<Object>(value);
            Reader<Object> reader = owner.reader();
            member = reader.getClass();
            require(owner.calls == 0 && owner.reader() == reader);
            require(owner.read() == value && reader.read() == value && owner.calls == 2);
            probes += 2;
            Object next = new Object();
            owner.value = next;
            require(reader.read() == next && owner.read() == next && owner.calls == 4);
            probes += 2;
            Method echo = member.getDeclaredMethod("echo", Object.class);
            echo.setAccessible(true);
            require(echo.invoke(reader, next) == next && owner.calls == 4);
            ++probes;
            require(echo.getTypeParameters().length == 1 && ((TypeVariable<?>) echo.getGenericReturnType()).getGenericDeclaration().equals(echo));
            RuntimeException sentinel = new RuntimeException("owned anonymous member failure");
            owner.failure = sentinel;
            try { reader.read(); throw new AssertionError("missing reader failure"); }
            catch (RuntimeException actual) { require(actual == sentinel && owner.calls == 5); ++failures; }
            try { owner.read(); throw new AssertionError("missing owner read failure"); }
            catch (RuntimeException actual) { require(actual == sentinel && owner.calls == 6); ++failures; }
        }
        Constructor<?> constructor = member.getDeclaredConstructor(Owner.class);
        constructor.setAccessible(true);
        Reader<?> empty = (Reader<?>) constructor.newInstance(new Object[] {null});
        require(empty.getClass() == member);
        try { empty.read(); throw new AssertionError("missing captured null owner failure"); }
        catch (NullPointerException expected) { ++failures; }
        Type contract = argument(member.getGenericInterfaces()[0]);
        Type result = member.getDeclaredMethod("read").getGenericReturnType();
        if (args[0].equals("original")) {
            require(member.isAnonymousClass() && member.getEnclosingClass() == Owner.class);
            require(contract instanceof TypeVariable && result instanceof TypeVariable
                    && ((TypeVariable<?>) contract).getGenericDeclaration() == Owner.class
                    && ((TypeVariable<?>) result).getGenericDeclaration() == Owner.class);
        } else {
            require(member.isMemberClass() && Modifier.isStatic(member.getModifiers()) && member.getDeclaringClass() == Owner.class);
            require(contract == Object.class && result == Object.class);
        }
        Type ownerResult = Owner.class.getDeclaredMethod("read").getGenericReturnType();
        require(ownerResult instanceof TypeVariable && ((TypeVariable<?>) ownerResult).getGenericDeclaration() == Owner.class);
        require(argument(Owner.class.getDeclaredMethod("reader").getGenericReturnType()).equals(ownerResult));
        System.out.println("returned-identities=" + probes + ";ordered-failures=" + failures + ";generic-differences=2");
    }
}
