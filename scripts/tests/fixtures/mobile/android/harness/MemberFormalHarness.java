import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class MemberFormalHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned member formal names"); }
    public static final class Payload implements Owner.Item {
        public int read() { Trace.reads++; Trace.hit(4); return 37; }
    }
    public static final class Other implements Owner.Item {
        public int read() { Trace.reads++; Trace.hit(4); return -19; }
    }
    static Object call(int route, boolean checked, Owner owner, Owner.Item input) throws Throwable {
        if (route == 0) return checked ? (Payload)owner.echo(input) : owner.echo(input);
        if (route == 1) return checked ? MemberFormalCaller.checked(owner,input) : MemberFormalCaller.raw(owner,input);
        if (route == 2) {
            try {
                Object result = Owner.class.getMethod("echo",Owner.Item.class).invoke(owner,input);
                return checked ? (Payload)result : result;
            } catch (InvocationTargetException error) { throw error.getCause(); }
        }
        MethodHandle target = MethodHandles.publicLookup().findVirtual(Owner.class,"echo",
            MethodType.methodType(Owner.Item.class,Owner.Item.class));
        Object result = target.invoke(owner,input);
        return checked ? (Payload)result : result;
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        String expectedName = args[3]; boolean checked = operation == 1;
        TypeVariable<?>[] formals = Owner.class.getTypeParameters();
        require(formals.length == 1 && formals[0].getName().equals(expectedName)
            && formals[0].getGenericDeclaration() == Owner.class && formals[0].getBounds().length == 1
            && formals[0].getBounds()[0] == Owner.Item.class);
        require(Owner.Item.class.getName().equals("fixture.Owner$Item") && Owner.Item.class.getDeclaringClass() == Owner.class);
        Method echo = Owner.class.getMethod("echo",Owner.Item.class);
        require(echo.getTypeParameters().length == 0 && echo.getGenericReturnType().equals(formals[0])
            && echo.getGenericParameterTypes()[0].equals(formals[0]));
        require(Owner.class.getField("value").getGenericType().equals(formals[0])
            && Owner.class.getConstructor(Owner.Item.class).getGenericParameterTypes()[0].equals(formals[0]));
        Method local = Owner.class.getMethod("local",Object.class);
        TypeVariable<?> methodFormal = local.getTypeParameters()[0];
        require(methodFormal.getName().equals("Item") && methodFormal.getGenericDeclaration().equals(local)
            && local.getGenericReturnType().equals(methodFormal) && local.getGenericParameterTypes()[0].equals(methodFormal));
        Payload seed = new Payload(); Owner<Payload> owner = mode == 5 ? null : new Owner<Payload>(seed);
        Owner.Item input = mode == 0 ? null : mode == 6 ? new Other() : seed;
        Object sentinel = new Object(); RuntimeException original = new IllegalStateException("owned original shadow failure");
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.receiver = Trace.last = sentinel; Trace.number = 987; Trace.failure = original;
            Trace.fail = mode >= 1 && mode <= 3 ? mode : mode == 4 ? 4 : 0;
            int reads = Trace.reads; Object before = owner == null ? null : owner.value;
            Throwable failure = null; Object actual = null;
            try { actual = call(route,checked,owner,input); } catch (Throwable error) { failure = error; }
            long events = mode == 5 ? 0 : mode == 1 ? 1 : mode == 2 || mode == 0 ? 12 : mode == 4 ? 124 : 1243;
            require(Trace.events == events && Trace.receiver == (mode == 5 ? sentinel : owner));
            boolean stored = mode != 1 && mode != 5, read = mode != 0 && mode != 1 && mode != 2 && mode != 5;
            require(Trace.last == (stored ? input : sentinel) && Trace.reads == reads + (read ? 1 : 0));
            if (owner != null) require(owner.value == (stored ? input : before));
            require(Trace.number == (read && mode != 4 ? mode == 6 ? -19 : 37 : 987));
            if (mode >= 1 && mode <= 4) require(failure == original);
            else if (mode == 0 || mode == 5) require(failure instanceof NullPointerException);
            else if (checked && mode == 6) require(failure instanceof ClassCastException && Trace.number == -19);
            else require(failure == null && actual == input);
            if (owner != null) require(owner.local(sentinel) == sentinel);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
