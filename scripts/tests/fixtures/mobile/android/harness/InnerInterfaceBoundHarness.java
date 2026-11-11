import fixture.Item;
import fixture.OtherItem;
import fixture.Owner;
import fixture.Value;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;

public final class InnerInterfaceBoundHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("Inner interface bound behavior changed");
    }
    private static Type argument(Type type) { return ((ParameterizedType) type).getActualTypeArguments()[0]; }
    public static void main(String[] args) throws Exception {
        int probes = 0, failures = 0;
        for (Object payload : new Object[] {new Object(), "owned", Integer.valueOf(73), null}) {
            Value item = new Value(payload);
            Object next = new Object();
            Value otherValue = new Value(next);
            OtherItem other = new OtherItem(otherValue);
            Owner<Item> owner = new Owner<Item>(item);
            Owner.Member member = owner.member(19);
            require(member.tag == 19 && member.value == item && owner.calls == 0);
            require(member.choose(item) == item && owner.calls == 1); ++probes;
            require(member.use(item) == payload && item.calls == 1 && owner.calls == 2); ++probes;
            require(member.echo(payload) == payload && owner.calls == 2); ++probes;
            require(member.choose(other) == other && member.value == other && owner.calls == 3); ++probes;
            require(member.use(other) == next && otherValue.calls == 1 && owner.calls == 4); ++probes;
            require(owner.outerRead() == item); ++probes;
            item.value = next;
            require(member.use(item) == next && item.calls == 2 && owner.calls == 5); ++probes;
            require(member.choose(null) == null && member.value == null && owner.calls == 6); ++probes;
            try { member.use(null); throw new AssertionError("missing null item failure"); }
            catch (NullPointerException expected) { require(owner.calls == 7 && item.calls == 2); ++failures; }
            RuntimeException sentinel = new RuntimeException("owned interface failure");
            item.failure = sentinel;
            try { member.use(item); throw new AssertionError("missing item failure"); }
            catch (RuntimeException actual) { require(actual == sentinel && owner.calls == 8 && item.calls == 3); ++failures; }
            otherValue.failure = sentinel;
            try { member.use(other); throw new AssertionError("missing delegated item failure"); }
            catch (RuntimeException actual) { require(actual == sentinel && owner.calls == 9 && otherValue.calls == 2); ++failures; }
        }
        Class<?> member = Owner.Member.class;
        try { member.getDeclaredConstructor(Owner.class, int.class).newInstance(null, 5); throw new AssertionError("missing null outer failure"); }
        catch (InvocationTargetException actual) { require(actual.getCause() instanceof NullPointerException); ++failures; }
        TypeVariable<?> formal = Owner.class.getTypeParameters()[0];
        require(formal.getBounds().length == 1 && formal.getBounds()[0] == Item.class);
        require(Owner.class.getDeclaredMethod("outerRead").getGenericReturnType().equals(formal));
        Method echo = member.getDeclaredMethod("echo", Object.class);
        require(echo.getTypeParameters().length == 1 && echo.getGenericReturnType().equals(echo.getTypeParameters()[0]));
        Type capture = argument(member.getDeclaredField("this$0").getGenericType());
        Field value = member.getDeclaredField("value");
        Method choose = member.getDeclaredMethod("choose", Item.class);
        Method use = member.getDeclaredMethod("use", Item.class);
        Type result = Owner.class.getDeclaredMethod("member", int.class).getGenericReturnType();
        if (args[0].equals("original")) {
            require(member.getDeclaringClass() == Owner.class && !Modifier.isStatic(member.getModifiers()));
            require(capture.equals(formal) && value.getGenericType().equals(formal));
            require(choose.getGenericParameterTypes()[0].equals(formal) && choose.getGenericReturnType().equals(formal));
            require(use.getGenericParameterTypes()[0].equals(formal));
            require(argument(((ParameterizedType) result).getOwnerType()).equals(formal));
        } else {
            require(member.getDeclaringClass() == Owner.class && Modifier.isStatic(member.getModifiers()));
            require(capture == Item.class && value.getGenericType() == Item.class);
            require(choose.getGenericParameterTypes()[0] == Item.class && choose.getGenericReturnType() == Item.class);
            require(use.getGenericParameterTypes()[0] == Item.class && result == member);
        }
        System.out.println("returned-identities=" + probes + ";ordered-failures=" + failures + ";captured-generic-differences=5");
    }
}
