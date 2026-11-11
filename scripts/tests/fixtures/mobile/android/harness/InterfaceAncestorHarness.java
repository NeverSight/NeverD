import fixture.*;
import java.lang.reflect.*;
import java.util.function.IntSupplier;

public class InterfaceAncestorHarness {
    private static int identities;
    private static int failures;
    private static int allocations;

    private static void check(boolean value) {
        if (!value) throw new AssertionError();
    }

    private static void same(Object actual, Object expected) {
        check(actual == expected);
        identities++;
    }

    private static void fails(Runnable action, Class<? extends Throwable> kind,
                              Throwable exact, IntSupplier calls, int expected) {
        try {
            action.run();
            throw new AssertionError("missing failure");
        } catch (Throwable error) {
            check(kind.isInstance(error));
            if (exact != null) check(error == exact);
            check(calls.getAsInt() == expected);
            failures++;
        }
    }

    private static void arguments(Type type, Class<?> raw, Type... expected) {
        check(type instanceof ParameterizedType);
        ParameterizedType actual = (ParameterizedType) type;
        check(actual.getRawType() == raw);
        Type[] found = actual.getActualTypeArguments();
        check(found.length == expected.length);
        for (int i = 0; i < expected.length; i++) check(found[i].equals(expected[i]));
    }

    private static void arrayType(Type type, Type component) {
        check(type instanceof GenericArrayType);
        check(((GenericArrayType) type).getGenericComponentType().equals(component));
    }

    private static void reflection() throws Exception {
        TypeVariable<?> root = Root.class.getTypeParameters()[0];
        TypeVariable<?> left = Left.class.getTypeParameters()[0];
        TypeVariable<?> right = Right.class.getTypeParameters()[0];
        TypeVariable<?> contract = Contract.class.getTypeParameters()[0];
        TypeVariable<?> other = Contract.class.getTypeParameters()[1];
        TypeVariable<?> core = Core.class.getTypeParameters()[0];
        TypeVariable<?> unused = Core.class.getTypeParameters()[1];
        check(root.getGenericDeclaration() == Root.class);
        check(left.getGenericDeclaration() == Left.class);
        check(right.getGenericDeclaration() == Right.class);
        check(contract.getGenericDeclaration() == Contract.class);
        check(core.getGenericDeclaration() == Core.class);
        check(!root.equals(left) && !left.equals(contract) && !contract.equals(core));
        arguments(Left.class.getGenericInterfaces()[0], Root.class, left);
        arguments(Right.class.getGenericInterfaces()[0], Root.class, right);
        arguments(Contract.class.getGenericInterfaces()[0], Left.class, contract);
        arguments(Contract.class.getGenericInterfaces()[1], Right.class, contract);
        arguments(Core.class.getGenericInterfaces()[0], Contract.class, core, unused);
        arguments(Closed.class.getGenericInterfaces()[0], Contract.class, Tag.class, Tag.class);
        check(other.getBounds()[0] == Object.class);
        check(core.getBounds()[0] == Tag.class && unused.getBounds()[0] == Tag.class);
        Method rootEcho = Root.class.getDeclaredMethod("echo", Object.class);
        check(rootEcho.getGenericReturnType().equals(root));
        check(rootEcho.getGenericParameterTypes()[0].equals(root));
        Method rootArray = Root.class.getDeclaredMethod("array", Object[].class);
        arrayType(rootArray.getGenericParameterTypes()[0], root);
        arrayType(rootArray.getGenericReturnType(), root);
        Method coreEcho = Core.class.getDeclaredMethod("echo", Tag.class);
        check(coreEcho.getGenericReturnType().equals(core));
        check(coreEcho.getGenericParameterTypes()[0].equals(core));
        check(Core.class.getDeclaredField("last").getGenericType().equals(core));
        Method coreArray = Core.class.getDeclaredMethod("array", Tag[].class);
        arrayType(coreArray.getGenericParameterTypes()[0], core);
        arrayType(coreArray.getGenericReturnType(), core);
        arrayType(Core.class.getDeclaredMethod("newArray", int.class).getGenericReturnType(), core);
        check(Closed.class.getDeclaredMethod("newArray", int.class).getGenericReturnType() == Tag[].class);
        for (Class<?> owner : new Class<?>[]{Core.class, Closed.class}) {
            int count = 0;
            for (Method method : owner.getDeclaredMethods()) {
                if (method.isBridge()) {
                    check(method.isSynthetic());
                    count++;
                }
            }
            check(count == 4);
        }
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Core<Specific, Tag> core = new Core<>();
        Specific a = new Specific();
        Specific b = new Specific();
        Root<Specific> root = core;
        Left<Specific> left = core;
        Right<Specific> right = core;
        Contract<Specific, Tag> contract = core;
        Root raw = core;
        for (Specific value : new Specific[]{a, b, null}) {
            same(core.echo(value), value);
            same(root.echo(value), value);
            same(left.echo(value), value);
            same(right.echo(value), value);
            same(contract.echo(value, 2), value);
            same(raw.echo(value), value);
            same(core.last, value);
        }
        Specific[] values = {a, b};
        same(raw.array(values), values);
        same(root.array(values), values);
        same(core.last, a);
        same(core.newArray(0), null);
        same(contract.newArray(-1), null);
        values[0] = b;
        same(right.array(values), values);
        same(core.last, b);
        int before = core.calls;
        fails(() -> raw.echo(new Object()), ClassCastException.class, null, () -> core.calls, before);
        fails(() -> raw.array(new Object[1]), ClassCastException.class, null, () -> core.calls, before);
        fails(() -> raw.array(null), NullPointerException.class, null, () -> core.calls, before + 1);
        before = core.calls;
        fails(() -> raw.array(new Specific[0]), ArrayIndexOutOfBoundsException.class, null,
              () -> core.calls, before + 1);
        RuntimeException sentinel = new RuntimeException("owned failure");
        core.failure = sentinel;
        before = core.calls;
        fails(() -> raw.echo(a), RuntimeException.class, sentinel, () -> core.calls, before + 1);
        check(core.last == a);
        before = core.calls;
        fails(() -> raw.echo(new Object()), ClassCastException.class, null, () -> core.calls, before);
        fails(() -> raw.array(null), RuntimeException.class, sentinel, () -> core.calls, before + 1);
        before = core.calls;
        fails(() -> contract.echo(b, Integer.MAX_VALUE), RuntimeException.class, sentinel,
              () -> core.calls, before + Integer.MAX_VALUE);
        check(core.last == b);
        before = core.calls;
        fails(() -> contract.newArray(-3), RuntimeException.class, sentinel, () -> core.calls, before - 3);
        core.failure = null;

        Closed closed = new Closed();
        Root<Tag> closedRoot = closed;
        Left<Tag> closedLeft = closed;
        Right<Tag> closedRight = closed;
        Contract<Tag, Tag> closedContract = closed;
        Root closedRaw = closed;
        for (Tag value : new Tag[]{new Tag(), a, null}) {
            same(closed.echo(value), value);
            same(closedRoot.echo(value), value);
            same(closedLeft.echo(value), value);
            same(closedRight.echo(value), value);
            same(closedContract.echo(value, 2), value);
            same(closedRaw.echo(value), value);
            same(closed.last, value);
        }
        Tag[] tags = {a};
        same(closedRoot.array(tags), tags);
        same(closed.last, a);
        for (int size : new int[]{0, 1, 3}) {
            Tag[] array = closedContract.newArray(size);
            check(array.getClass() == Tag[].class && array.length == size);
            for (Tag value : array) check(value == null);
            allocations++;
        }
        before = closed.calls;
        fails(() -> closedRaw.echo(new Object()), ClassCastException.class, null,
              () -> closed.calls, before);
        fails(() -> closedRaw.array(new Object[1]), ClassCastException.class, null,
              () -> closed.calls, before);
        fails(() -> closedRaw.array(null), NullPointerException.class, null,
              () -> closed.calls, before + 1);
        before = closed.calls;
        fails(() -> closedRaw.array(new Tag[0]), ArrayIndexOutOfBoundsException.class, null,
              () -> closed.calls, before + 1);
        before = closed.calls;
        fails(() -> closedContract.newArray(-1), NegativeArraySizeException.class, null,
              () -> closed.calls, before + 1);
        closed.failure = sentinel;
        before = closed.calls;
        fails(() -> closedContract.newArray(-1), RuntimeException.class, sentinel,
              () -> closed.calls, before + 1);
        before = closed.calls;
        fails(() -> closedRaw.echo(null), RuntimeException.class, sentinel,
              () -> closed.calls, before + 1);
        check(closed.last == null);
        before = closed.calls;
        fails(() -> closedContract.echo(b, Integer.MIN_VALUE), RuntimeException.class, sentinel,
              () -> closed.calls, before + Integer.MIN_VALUE);

        Core<Specific, Tag> virtual = new Core<Specific, Tag>() {
            @Override public Specific echo(Specific value) {
                calls++;
                return super.echo(value);
            }
        };
        Root virtualRoot = virtual;
        same(virtualRoot.echo(a), a);
        check(virtual.calls == 2);
        fails(() -> virtualRoot.echo(new Tag()), ClassCastException.class, null,
              () -> virtual.calls, 2);
        reflection();
        if (args.length > 0 && args[0].equals("marker-chain")) {
            Class<?> marker = Class.forName("fixture.Marker");
            Class<?> markerRoot = Class.forName("fixture.MarkerRoot");
            check(marker.isInstance(core) && markerRoot.isInstance(closed));
            check(Root.class.getInterfaces().length == 1 && Root.class.getInterfaces()[0] == marker);
            check(marker.getInterfaces().length == 1 && marker.getInterfaces()[0] == markerRoot);
            check(marker.getDeclaredMethods().length == 0 && markerRoot.getDeclaredMethods().length == 0);
        }
        System.out.println("identities=" + identities + ",failures=" + failures
                           + ",bridges=8,allocations=" + allocations);
        System.out.println("generic-ancestor-graph=retained,scopes=retained,diamond-dispatch=matched");
    }
}
